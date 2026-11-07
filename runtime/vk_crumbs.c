// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;

// NVIDIA (VK_NV_device_diagnostic_checkpoints, DKB_GPU_CHECKPOINTS=1): every pipeline bind sets a checkpoint whose marker
// is the VkPipeline; after a device loss the queue reports the checkpoints the GPU reached. The registry maps a pipeline
// to the NVN 2 names of its stages (what the bind site knows), filled at bind time, so the report names the shaders.
static PFN_vkCmdSetCheckpointNV set_cp;
static PFN_vkGetQueueCheckpointDataNV get_cp;
static PFN_vkGetQueueCheckpointData2NV get_cp2;
// Handles cannot be reused while the registry is read, only because bound pipelines live until vk_pipeline_shutdown.
// When programs share one VkPipeline the last bind wins. Work after the last bind (clear, ASTC, FSR, copies) is
// attributed to that pipeline.
enum { CP_SLOTS = 8192, CP_PROBES = 64 };   // ponytail: open addressing, a full table drops new pipelines; enlarge if the report shows "unknown"
static struct { VkPipeline pipe; uint64_t stage[3]; } cp_reg[CP_SLOTS];   // vertex, geometry, fragment (or compute, 0, 0)
static PlatMutex cp_lock = PLAT_MUTEX_INIT;
static volatile uint32_t cp_tag_n;   // next tag slot, see vk_checkpoint_tag

// High bits of a Fibonacci multiply (pointers have constant low bits), as find_stage in vk_exec_draw.c.
static uint32_t cp_slot(const void *p) { return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 51); }

// Device address bindings (VK_EXT_device_address_binding_report, with the checkpoints on): the driver reports every
// range of GPU virtual addresses it binds or unbinds to a debug utils messenger. The newest AB_RING events are kept so
// that the report can name what a faulting address belonged to. Sequence numbers count every event, so a wrapped ring
// shows how many are gone.
enum { AB_RING = 4096 };
typedef struct { uint64_t seq, base, size, handle; uint32_t unbind, flags, type; } AbEvent;
static AbEvent ab_ring[AB_RING];
static uint64_t ab_n;
static VkDebugUtilsMessengerEXT ab_messenger;

static VKAPI_ATTR VkBool32 VKAPI_CALL ab_callback(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                  VkDebugUtilsMessageTypeFlagsEXT type,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)sev, (void)type, (void)user;
    for (const VkBaseInStructure *s = d->pNext; s; s = s->pNext) {
        if (s->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
        const VkDeviceAddressBindingCallbackDataEXT *b = (const void *)s;
        uint64_t n = __atomic_fetch_add(&ab_n, 1, __ATOMIC_RELAXED);
        AbEvent *e = &ab_ring[n % AB_RING];
        // Seqlock: seq 0 while the slot is rewritten, n + 1 once complete; a reader skips a slot whose seq changed.
        __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        e->base = b->baseAddress, e->size = b->size, e->unbind = b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT;
        e->flags = b->flags;
        e->type = d->objectCount ? (uint32_t)d->pObjects[0].objectType : 0;
        e->handle = d->objectCount ? d->pObjects[0].objectHandle : 0;
        __atomic_store_n(&e->seq, n + 1, __ATOMIC_RELEASE);   // 0 also marks a slot never written
    }
    return VK_FALSE;
}

static const char *ab_object(uint32_t t) {
    return t == VK_OBJECT_TYPE_DEVICE_MEMORY ? "memory" : t == VK_OBJECT_TYPE_BUFFER ? "buffer" : t == VK_OBJECT_TYPE_IMAGE ? "image" : "object";
}

static void ab_print(const char *why, const AbEvent *e) {
    rt_log("vk: address binding #%llu %s: %s %#llx..%#llx (%llu bytes) %s %#llx%s", (unsigned long long)e->seq, why,
           e->unbind ? "unbind" : "bind", (unsigned long long)e->base, (unsigned long long)(e->base + e->size),
           (unsigned long long)e->size, ab_object(e->type), (unsigned long long)e->handle,
           e->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT ? " (driver internal)" : "");
}

void vk_crumbs_report_address(uint64_t a) {
    if (!ab_messenger) return;
    uint64_t total = __atomic_load_n(&ab_n, __ATOMIC_ACQUIRE);
    uint64_t first = total > AB_RING ? total - AB_RING : 0;
    uint32_t shown = 0;
    const AbEvent *near = NULL;
    rt_log("vk: address %#llx: %llu binding events seen, the newest %llu kept", (unsigned long long)a,
           (unsigned long long)total, (unsigned long long)(total - first));
    static AbEvent keep;   // copy of the nearest candidate, so later rewrites of its slot do not matter
    for (uint64_t s = first; s < total; s++) {   // oldest first, so an unbind after a bind reads in order
        const AbEvent *slot = &ab_ring[s % AB_RING];
        if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != s + 1) continue;
        AbEvent copy = *slot;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&slot->seq, __ATOMIC_RELAXED) != s + 1) continue;   // rewritten while copying
        const AbEvent *e = &copy;
        if (a >= e->base && a - e->base < e->size) {
            if (shown++ < 16) ab_print("contains the address", e);
        } else if (!e->unbind && e->base + e->size <= a && a - (e->base + e->size) < (1u << 20) &&
                   (!near || e->base + e->size >= near->base + near->size)) {
            keep = copy, near = &keep;
        }
    }
    if (!shown && near) ab_print("nearest bind below the address (within 1 MiB)", near);
    else if (!shown)
        rt_log("vk: address %#llx: no binding contains it or ends within 1 MiB below it%s", (unsigned long long)a,
               total > AB_RING ? " (ring wrapped: older bindings are gone)" : "");
    // ponytail: the nearest-below search looks at binds only and ignores later unbinds of them; read the sequence numbers.
}

void vk_crumbs_init(VkCtx *c) {
    if (c->has_address_binding_report) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(c->instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
            .pfnUserCallback = ab_callback};
        if (f && f(c->instance, &mi, NULL, &ab_messenger) == VK_SUCCESS)
            rt_log("vk: device address bindings recorded (VK_EXT_device_address_binding_report)");
        else rt_log("vk: device address binding report unavailable");
    } else if (c->has_nv_checkpoints) {
        rt_log("vk: device address binding report not available (VK_EXT_device_address_binding_report): fault addresses stay unmapped");
    }
    if (c->has_nv_checkpoints) {
        set_cp = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(c->device, "vkCmdSetCheckpointNV");
        get_cp = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointDataNV");
        get_cp2 = (PFN_vkGetQueueCheckpointData2NV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointData2NV");
        if (!set_cp || !(get_cp || get_cp2)) set_cp = NULL;
        rt_log(set_cp ? "vk: GPU checkpoints on (VK_NV_device_diagnostic_checkpoints)" : "vk: GPU checkpoints unavailable");
    }
    if (!c->has_buffer_marker) return;
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (ab_messenger && c) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(c->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(c->instance, ab_messenger, NULL);
    }
    ab_messenger = VK_NULL_HANDLE;
    ab_n = 0;
    memset(ab_ring, 0, sizeof ab_ring);
    cp_tag_n = 0;
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
    set_cp = NULL, get_cp = NULL, get_cp2 = NULL;
    plat_mutex_lock(&cp_lock);
    memset(cp_reg, 0, sizeof cp_reg);
    plat_mutex_unlock(&cp_lock);
}

int vk_checkpoints_on(void) { return set_cp != NULL; }

void vk_checkpoint(VkCommandBuffer cb, const void *marker) {
    if (set_cp && cb) set_cp(cb, marker);
}

void vk_checkpoint_note(VkPipeline p, uint64_t a, uint64_t b, uint64_t c) {
    if (!set_cp || !p) return;
    plat_mutex_lock(&cp_lock);
    uint32_t i = cp_slot(p);
    int n = 0;
    while (n < CP_PROBES && cp_reg[i].pipe && cp_reg[i].pipe != p) n++, i = (i + 1) % CP_SLOTS;
    if (n < CP_PROBES) cp_reg[i].pipe = p, cp_reg[i].stage[0] = a, cp_reg[i].stage[1] = b, cp_reg[i].stage[2] = c;
    plat_mutex_unlock(&cp_lock);
}

// Non-pipeline checkpoints (dispatch, copies, blits, clears; vk_rec.c): the marker points into this ring and names the
// command, plus the compute pipeline last bound on the recording thread for a dispatch. The ring wraps: a checkpoint
// older than CP_TAGS tags would read a newer tag, far more than the frames in flight hold.
enum { CP_TAGS = 1 << 16 };
static struct { const char *kind; VkPipeline compute; } cp_tags[CP_TAGS];

static _Thread_local VkPipeline cp_compute;

// The last-bound pipeline is per recording thread, not per command buffer, so a dispatch may carry the pipeline another
// buffer bound last on that thread (diagnostic only).
void vk_checkpoint_compute(VkPipeline p) { cp_compute = p; }

const void *vk_checkpoint_tag(const char *kind) {
    if (!set_cp) return NULL;
    uint32_t i = __atomic_fetch_add(&cp_tag_n, 1, __ATOMIC_RELAXED) % CP_TAGS;
    cp_tags[i].kind = kind;
    cp_tags[i].compute = !strncmp(kind, "dispatch", 8) ? cp_compute : VK_NULL_HANDLE;
    return &cp_tags[i];
}

static void pipeline_name(const void *m, char *o, size_t size);

static void checkpoint_name(const void *m, char *o, size_t size) {
    if ((const char *)m >= (const char *)cp_tags && (const char *)m < (const char *)(cp_tags + CP_TAGS)) {
        int n = snprintf(o, size, "%s", ((const typeof(cp_tags[0]) *)m)->kind);
        if (((const typeof(cp_tags[0]) *)m)->compute && n > 0 && (size_t)n < size) {
            snprintf(o + n, size - (size_t)n, ", last bound compute ");
            n = (int)strlen(o);
            pipeline_name(((const typeof(cp_tags[0]) *)m)->compute, o + n, size - (size_t)n);
        }
        return;
    }
    pipeline_name(m, o, size);
}

static void pipeline_name(const void *m, char *o, size_t size) {
    snprintf(o, size, "marker %p, pipeline not registered", m);
    plat_mutex_lock(&cp_lock);
    uint32_t i = cp_slot(m);
    for (int n = 0; n < CP_PROBES && cp_reg[i].pipe; n++, i = (i + 1) % CP_SLOTS)
        if ((const void *)cp_reg[i].pipe == m) {
            snprintf(o, size, "pipeline %p, NVN 2 stages %016llx %016llx %016llx (vertex or compute, geometry, fragment)", m,
                     (unsigned long long)cp_reg[i].stage[0], (unsigned long long)cp_reg[i].stage[1],
                     (unsigned long long)cp_reg[i].stage[2]);
            break;
        }
    plat_mutex_unlock(&cp_lock);
}

static void checkpoints_report(void) {
    VkCheckpointData2NV d2[16];
    VkCheckpointDataNV d1[16];
    uint32_t n = 16;
    VkCtx *c = vk_ctx();
    if (!c) return;
    VkQueue q = c->gfx_queue;
    if (get_cp2) {
        for (int i = 0; i < 16; i++) d2[i] = (VkCheckpointData2NV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        get_cp2(q, &n, d2);
    } else {
        for (int i = 0; i < 16; i++) d1[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
        get_cp(q, &n, d1);
    }
    rt_log("vk: GPU checkpoints: %u reported on the graphics queue", n);
    for (uint32_t i = 0; i < n && i < 16; i++) {
        char name[256];
        const void *m = get_cp2 ? d2[i].pCheckpointMarker : d1[i].pCheckpointMarker;
        checkpoint_name(m, name, sizeof name);
        unsigned long long st = get_cp2 ? (unsigned long long)d2[i].stage : (unsigned long long)d1[i].stage;
        if (st == 0x1) rt_log("vk: GPU checkpoint %u: stage TOP_OF_PIPE, %s", i, name);
        else if (st == 0x2000) rt_log("vk: GPU checkpoint %u: stage BOTTOM_OF_PIPE, %s", i, name);
        else rt_log("vk: GPU checkpoint %u: stage %#llx, %s", i, st, name);
    }
}

void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) {
    if (!write_marker || !cb) return;
    uint32_t code = (uint32_t)kind << 28 | (uint32_t)(n & 0x0fffffff);
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode chunk", "the start of present",
                                        "the end of present"};
    unsigned k = code >> 28;
    if (!code) snprintf(o, size, "nothing yet");
    else if (k < sizeof kinds / sizeof *kinds) snprintf(o, size, "%s %u", kinds[k], code & 0x0fffffff);
    else snprintf(o, size, "unknown code %#x", code);
}

void vk_crumbs_report(void) {
    if (set_cp) checkpoints_report();
    if (!words) return;
    char reached[96], finished[96];
    describe(words[0], reached, sizeof reached);
    describe(words[1], finished, sizeof finished);
    rt_log("vk: GPU breadcrumbs: the GPU last reached %s; everything before %s had finished", reached, finished);
}
