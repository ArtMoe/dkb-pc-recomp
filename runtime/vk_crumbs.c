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

// High bits of a Fibonacci multiply (pointers have constant low bits), as find_stage in vk_exec_draw.c.
static uint32_t cp_slot(const void *p) { return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 51); }

void vk_crumbs_init(VkCtx *c) {
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

static void checkpoint_name(const void *m, char *o, size_t size) {
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
