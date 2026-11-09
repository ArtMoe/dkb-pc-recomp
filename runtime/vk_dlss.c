// SPDX-License-Identifier: GPL-2.0-or-later
// DLSS Super Resolution in place of the game's own upscaler (upscaler = dlss, experimental, NVIDIA RTX only). The
// game scales its graded frame to the output size with a compute dispatch (vk_exec_draw.c dlss_upscale picks it by
// the hash of its SPIR-V); here NVIDIA NGX evaluates DLSS on the same input (texture slot 0, the render resolution)
// into the same output (image slot 1) instead. Slice 1 is spatial only: no jitter, zero motion vectors, a zero depth
// image and Reset = 1 on every frame, because the game has no velocity target or history to borrow (bananza-plan.md,
// "Later"). Any failure logs one "dlss: <reason>" line and the game's dispatch runs from then on.
//
// NGX itself: the core (_nvngx.dll) comes with the NVIDIA driver and is found through the registry keys NVIDIA's
// header-only loader reads; the DLSS model (nvngx_dlss.dll) is the user's and sits next to dkb.exe (docs/dkb-dlss.md).
// The core is called through its C exports. Its parameter object is a C++ class built by MSVC, so its virtual methods
// go through the MSVC vtable layout (overloads in reverse declaration order: Set void* 0, int 3, unsigned 4, float 6;
// Get int* 11). No NVIDIA header or binary is in this repository: the few declarations below carry the values of
// NVIDIA's DLSS SDK 310.9 (nvsdk_ngx_defs.h, nvsdk_ngx_defs_vk.h, nvsdk_ngx_params.h).
//
// Derived from the NGX-direct DLSS path of the Shadow of the Colossus PC fork (namespace Ngx in
// gpu/shadps4/video_core/renderer_vulkan/vk_dlssg.cpp, https://github.com/Lucas-blip-png/ShadowOfTheColossus-PC
// @ 7184fb84e341b24fff824b6fc16d3341fda41828, GPL-2.0-or-later), rewritten in C for this runtime.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef uint32_t NgxResult;
#define NGX_FAILED(r) (((r) & 0xFFF00000u) == 0xBAD00000u)
enum { NGX_VERSION_API = 0x15, NGX_FEATURE_SUPER_SAMPLING = 1 };
enum { Q_MAX_PERF, Q_BALANCED, Q_MAX_QUALITY, Q_ULTRA_PERF, Q_ULTRA_QUALITY, Q_DLAA };
enum { F_MV_LOW_RES = 1 << 1, F_AUTO_EXPOSURE = 1 << 6 };
enum { PRESET_L = 12, PRESET_M = 13 };   // NGX render preset hints
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct {   // NVSDK_NGX_Resource_VK holding an image view (the larger member of its union)
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width, height;
    uint32_t type;   // 0: image view
    uint8_t read_write;
} NgxResourceVk;

typedef NgxResult (*PfnRequired)(unsigned *, const char ***, unsigned *, const char ***);
typedef NgxResult (*PfnInit)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                             PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, uint32_t, const void *);
typedef NgxResult (*PfnParams)(NgxParam **);
typedef NgxResult (*PfnCreate)(VkDevice, VkCommandBuffer, uint32_t, NgxParam *, NgxHandle **);
typedef NgxResult (*PfnEvaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *);
typedef NgxResult (*PfnRelease)(NgxHandle *);
typedef NgxResult (*PfnShutdown)(VkDevice);
typedef NgxResult (*PfnDestroy)(NgxParam *);

static HMODULE dll;
static PfnInit ngx_init;
static PfnParams ngx_caps, ngx_alloc;
static PfnCreate ngx_create;
static PfnEvaluate ngx_eval;
static PfnRelease ngx_release;
static PfnShutdown ngx_shutdown;
static PfnDestroy ngx_destroy;
static const char **inst_ext, **dev_ext;
static unsigned n_inst_ext, n_dev_ext;
static int state;   // 0 not tried, 1 ready, -1 failed (the game's upscaler from then on)
static NgxParam *params;

// The feature and its zero inputs, made for one input and output size.
static struct {
    NgxHandle *h;
    uint32_t w, h_, ow, oh;
    VkImage mv, depth;
    VmaAllocation mv_a, depth_a;
    VkImageView mv_v, depth_v;
} F;
// Features replaced while the GPU may still run them: released once their submission's fence is done.
#define GRAVE 8
static struct { NgxHandle *h; uint64_t fence; } grave[GRAVE];

static void *slot(const NgxParam *p, int i) { return (*(void *const *const *)p)[i]; }
static void set_ptr(NgxParam *p, const char *n, void *v) { ((void (*)(NgxParam *, const char *, void *))slot(p, 0))(p, n, v); }
static void set_i(NgxParam *p, const char *n, int v) { ((void (*)(NgxParam *, const char *, int))slot(p, 3))(p, n, v); }
static void set_u(NgxParam *p, const char *n, unsigned v) { ((void (*)(NgxParam *, const char *, unsigned))slot(p, 4))(p, n, v); }
static void set_f(NgxParam *p, const char *n, float v) { ((void (*)(NgxParam *, const char *, float))slot(p, 6))(p, n, v); }
static int get_i(NgxParam *p, const char *n) {
    int v = 0;
    ((NgxResult (*)(NgxParam *, const char *, int *))slot(p, 11))(p, n, &v);
    return v;
}

static int fail(const char *why) {
    if (state >= 0) rt_log("dlss: %s; the game's upscaler runs", why);
    state = -1;
    return 0;
}

int vk_dlss_wanted(void) {
    static int w = -1;
    if (w < 0) w = !strcmp(cfg_value("TOTK_UPSCALER"), "dlss");
    return w && state >= 0;
}

// Before the instance: finds _nvngx.dll and asks it for its extensions.
static int load(void) {
    static int tried;
    if (tried) return dll != NULL;
    tried = 1;
    wchar_t path[MAX_PATH + 16];
    static const wchar_t *const keys[][2] = {
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath"},
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
        {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"},
    };
    int found = 0;
    for (int i = 0; i < 3 && !found; i++) {
        DWORD size = MAX_PATH * sizeof(wchar_t);
        found = RegGetValueW(HKEY_LOCAL_MACHINE, keys[i][0], keys[i][1], RRF_RT_REG_SZ, NULL, path, &size) == ERROR_SUCCESS;
    }
    if (!found) return fail("no NVIDIA NGX core in the registry (no NVIDIA driver?)");
    wcscat(path, L"\\_nvngx.dll");
    dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return fail("the driver's _nvngx.dll does not load");
#define GET(v, name) (v = (__typeof__(v))(void *)GetProcAddress(dll, name)) != NULL
    PfnRequired required;
    if (!(GET(required, "NVSDK_NGX_VULKAN_RequiredExtensions") && GET(ngx_init, "NVSDK_NGX_VULKAN_Init_Ext2") &&
          GET(ngx_caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters") && GET(ngx_alloc, "NVSDK_NGX_VULKAN_AllocateParameters") &&
          GET(ngx_create, "NVSDK_NGX_VULKAN_CreateFeature1") && GET(ngx_eval, "NVSDK_NGX_VULKAN_EvaluateFeature") &&
          GET(ngx_release, "NVSDK_NGX_VULKAN_ReleaseFeature"))) {
        FreeLibrary(dll);
        dll = NULL;
        return fail("_nvngx.dll lacks an export DLSS needs");
    }
    GET(ngx_shutdown, "NVSDK_NGX_VULKAN_Shutdown1");
    GET(ngx_destroy, "NVSDK_NGX_VULKAN_DestroyParameters");
#undef GET
    if (NGX_FAILED(required(&n_inst_ext, &inst_ext, &n_dev_ext, &dev_ext))) n_inst_ext = n_dev_ext = 0;
    rt_log("dlss: NGX core loaded, it asks for %u instance and %u device extensions", n_inst_ext, n_dev_ext);
    return 1;
}

int vk_dlss_instance_exts(const char **out, int max) {
    if (!vk_dlss_wanted() || !load()) return 0;
    int n = 0;
    for (unsigned i = 0; i < n_inst_ext && n < max; i++) out[n++] = inst_ext[i];
    return n;
}

int vk_dlss_device_exts(const char **out, int max) {
    if (!vk_dlss_wanted() || !dll) return 0;
    int n = 0;
    for (unsigned i = 0; i < n_dev_ext && n < max; i++) out[n++] = dev_ext[i];
    return n;
}

// After the device, at the first upscale: NGX initialised and DLSS Super Resolution available.
static int init(void) {
    if (state) return state > 0;
    if (!dll) return fail("the NGX core was not loaded before the device");
    VkCtx *c = vk_ctx();
    if (c->props.vendorID != 0x10de) return fail("the Vulkan device is not an NVIDIA GPU");
    wchar_t dir[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *cut = n ? wcsrchr(dir, L'\\') : NULL;
    if (!cut) return fail("the executable's folder is unknown");
    wcscpy(cut + 1, L"nvngx_dlss.dll");
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return fail("nvngx_dlss.dll is not next to dkb.exe");
    *cut = 0;
    // 231313132: the application id of NVIDIA's DLSS samples, for applications without their own. The data path is
    // where NGX looks for nvngx_dlss.dll first and writes its logs.
    NgxResult r = ngx_init(231313132ull, dir, c->instance, c->phys, c->device, vkGetInstanceProcAddr,
                           vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    char why[96];
    if (NGX_FAILED(r)) {
        snprintf(why, sizeof why, "NGX initialisation failed (%#x)", r);
        return fail(why);
    }
    // From here a failure shuts NGX down again (its capability and parameter objects first).
    NgxParam *caps = NULL;
    r = ngx_caps(&caps);
    if (NGX_FAILED(r) || !caps) snprintf(why, sizeof why, "NGX has no capability parameters (%#x)", r);
    else if (!get_i(caps, "SuperSampling.Available"))
        snprintf(why, sizeof why, "DLSS is not available on this GPU (newer driver needed: %d)",
                 get_i(caps, "SuperSampling.NeedsUpdatedDriver"));
    else if (NGX_FAILED(ngx_alloc(&params)) || !params) snprintf(why, sizeof why, "NGX gave no parameter object");
    else why[0] = 0;
    if (caps && ngx_destroy) ngx_destroy(caps);
    if (why[0]) {
        if (params && ngx_destroy) ngx_destroy(params);
        params = NULL;
        if (ngx_shutdown) ngx_shutdown(c->device);
        return fail(why);
    }
    state = 1;
    return 1;
}

static void reap(void) {
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < GRAVE; i++)
        if (grave[i].h && grave[i].fence <= done) ngx_release(grave[i].h), grave[i].h = NULL;
}

static void drop_inputs(void) {
    vk_defer_view(F.mv_v), vk_defer_view(F.depth_v);
    vk_defer_image(F.mv, F.mv_a), vk_defer_image(F.depth, F.depth_a);
    F.mv = F.depth = VK_NULL_HANDLE, F.mv_v = F.depth_v = VK_NULL_HANDLE, F.mv_a = F.depth_a = NULL;
}

static VkImageView view(VkImage img, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageViewUsageCreateInfo u = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &u, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

// A w x h image of fmt in SHADER_READ_ONLY_OPTIMAL, cleared to zero by commands recorded into cb (through the recorder, before it drains).
static int zero_image(VkCommandBuffer cb, VkFormat fmt, uint32_t w, uint32_t h, VkImage *img, VmaAllocation *a,
                      VkImageView *v) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ii, &ai, img, a, NULL) != VK_SUCCESS) return 0;
    *v = view(*img, fmt, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!*v) return 0;
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = *img, .subresourceRange = rg};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue zero = {{0}};
    vkCmdClearColorImage(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rg);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
    return 1;
}

// The mode follows the scale: 1.5x quality, 1.7x balanced, 2x performance, 3x ultra performance, 1x DLAA.
static int quality(uint32_t w, uint32_t ow) {
    float s = (float)ow / (float)w;
    return s < 1.1f ? Q_DLAA : s < 1.6f ? Q_MAX_QUALITY : s < 1.85f ? Q_BALANCED : s < 2.5f ? Q_MAX_PERF : Q_ULTRA_PERF;
}

// A feature for w x h -> ow x oh, recorded into cb. A dynamic resolution change makes the game render into a new
// texture of another size, and every new size recreates the feature here: a hitch of a few milliseconds each time.
// ponytail: one feature per size; create it once at the largest render size and pass the size per frame through
// DLSS.Render.Subrect.Dimensions if those hitches show.
// 1 ready; 0 nothing recorded by NGX; -1 NGX failed after it may have recorded into cb.
static int feature(VkCommandBuffer cb, uint64_t fence, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    if (F.h && F.w == w && F.h_ == h && F.ow == ow && F.oh == oh) return 1;
    reap();
    if (F.h) {
        int i = 0;
        while (i < GRAVE && grave[i].h) i++;
        if (i == GRAVE) return 0;   // every slot still in flight: the game's upscaler this frame, a new try next frame
        grave[i].h = F.h, grave[i].fence = fence;
        F.h = NULL;
        drop_inputs();
    }
    int q = quality(w, ow);
    set_u(params, "CreationNodeMask", 1);
    set_u(params, "VisibilityNodeMask", 1);
    set_u(params, "Width", w);
    set_u(params, "Height", h);
    set_u(params, "OutWidth", ow);
    set_u(params, "OutHeight", oh);
    set_i(params, "PerfQualityValue", q);
    set_i(params, "DLSS.Feature.Create.Flags", F_MV_LOW_RES | F_AUTO_EXPOSURE);
    set_i(params, "DLSS.Enable.Output.Subrects", 0);
    // DLSS 4.5's second transformer model (preset M) in every mode but Ultra Performance, which has its own (L).
    set_u(params, "DLSS.Hint.Render.Preset.DLAA", PRESET_M);
    set_u(params, "DLSS.Hint.Render.Preset.Quality", PRESET_M);
    set_u(params, "DLSS.Hint.Render.Preset.Balanced", PRESET_M);
    set_u(params, "DLSS.Hint.Render.Preset.Performance", PRESET_M);
    set_u(params, "DLSS.Hint.Render.Preset.UltraPerformance", PRESET_L);
    if (!zero_image(cb, VK_FORMAT_R16G16_SFLOAT, w, h, &F.mv, &F.mv_a, &F.mv_v) ||
        !zero_image(cb, VK_FORMAT_R32_SFLOAT, w, h, &F.depth, &F.depth_a, &F.depth_v)) {
        drop_inputs();
        return fail("its zero motion-vector and depth images could not be made");
    }
    rec_drain();   // NGX records into cb on this thread: everything queued for the recorder first
    NgxResult r = ngx_create(vk_ctx()->device, cb, NGX_FEATURE_SUPER_SAMPLING, params, &F.h);
    if (NGX_FAILED(r) || !F.h) {
        char why[96];
        snprintf(why, sizeof why, "creating DLSS %ux%u -> %ux%u failed (%#x)", w, h, ow, oh, r);
        F.h = NULL;
        drop_inputs();
        fail(why);
        return -1;
    }
    F.w = w, F.h_ = h, F.ow = ow, F.oh = oh;
    static const char *const names[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
    rt_log("dlss: %s %ux%u -> %ux%u, preset %s, spatial only (no jitter or motion vectors)", names[q], w, h, ow, oh,
           q == Q_ULTRA_PERF ? "L" : "M");
    return 1;
}

int vk_dlss_record(VkCommandBuffer cb, uint64_t fence, const VkTexImage *in, const VkTexImage *out) {
    // The setting is live: fsr or bilinear chosen while the game runs turns DLSS off here (the extensions stay).
    if (!vk_dlss_wanted() || strcmp(cfg_value("TOTK_UPSCALER"), "dlss") || !init()) return 0;
    uint32_t w = in->width, h = in->height, ow = out->width, oh = out->height;
    if (w < 32 || h < 32 || w > ow || h > oh) {   // this pair of images only: the game's upscaler runs for it
        static int logged;
        if (!logged++) rt_log("dlss: unsupported size %ux%u -> %ux%u: the game's upscaler runs for it", w, h, ow, oh);
        return 0;
    }
    // Views of the game's images for this frame only: they go when the submission is done.
    VkImageView iv = view(in->image, in->format, VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView ov = view(out->image, out->format, VK_IMAGE_USAGE_STORAGE_BIT);
    if (!iv || !ov) {
        vk_defer_view(iv), vk_defer_view(ov);
        return fail("no sampled view of the input or storage view of the output");
    }
    int made = feature(cb, fence, w, h, ow, oh);
    if (made <= 0) {
        vk_defer_view(iv), vk_defer_view(ov);
        return made;
    }
    VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    NgxResourceVk color = {iv, in->image, rg, in->format, w, h, 0, 0};
    NgxResourceVk output = {ov, out->image, rg, out->format, ow, oh, 0, 1};
    NgxResourceVk depth = {F.depth_v, F.depth, rg, VK_FORMAT_R32_SFLOAT, w, h, 0, 0};
    NgxResourceVk mv = {F.mv_v, F.mv, rg, VK_FORMAT_R16G16_SFLOAT, w, h, 0, 0};
    set_ptr(params, "Color", &color);
    set_ptr(params, "Output", &output);
    set_ptr(params, "Depth", &depth);
    set_ptr(params, "MotionVectors", &mv);
    set_f(params, "Jitter.Offset.X", 0.0f);
    set_f(params, "Jitter.Offset.Y", 0.0f);
    set_f(params, "Sharpness", 0.0f);
    set_i(params, "Reset", 1);   // no history worth keeping without motion vectors
    set_f(params, "MV.Scale.X", 1.0f);
    set_f(params, "MV.Scale.Y", 1.0f);
    set_u(params, "DLSS.Render.Subrect.Dimensions.Width", w);
    set_u(params, "DLSS.Render.Subrect.Dimensions.Height", h);
    set_f(params, "DLSS.Pre.Exposure", 1.0f);
    set_f(params, "DLSS.Exposure.Scale", 1.0f);
    rec_drain();   // the game's images are in GENERAL (vk_texture.c); the caller's full barrier comes before this
    NgxResult r = ngx_eval(cb, F.h, params, NULL);
    vk_defer_view(iv), vk_defer_view(ov);
    if (NGX_FAILED(r)) {
        char why[64];
        snprintf(why, sizeof why, "evaluating DLSS failed (%#x)", r);
        fail(why);
        return -1;
    }
    return 1;
}

void vk_dlss_release(void) {   // device idle
    if (!params) return;   // NGX never initialised
    for (int i = 0; i < GRAVE; i++)
        if (grave[i].h) ngx_release(grave[i].h), grave[i].h = NULL;
    if (F.h) ngx_release(F.h), F.h = NULL;
    VkCtx *c = vk_ctx();
    if (c) {
        if (F.mv_v) vkDestroyImageView(c->device, F.mv_v, NULL);
        if (F.depth_v) vkDestroyImageView(c->device, F.depth_v, NULL);
        if (F.mv) vmaDestroyImage(c->vma, F.mv, F.mv_a);
        if (F.depth) vmaDestroyImage(c->vma, F.depth, F.depth_a);
        if (ngx_destroy) ngx_destroy(params);
        if (ngx_shutdown) ngx_shutdown(c->device);
    }
    memset(&F, 0, sizeof F);
    params = NULL;
    if (state > 0) state = 0;   // a new device initialises NGX again; a failure stays a failure
}

#else   // NGX is Windows-only here

int vk_dlss_wanted(void) { return 0; }
int vk_dlss_instance_exts(const char **out, int max) { (void)out, (void)max; return 0; }
int vk_dlss_device_exts(const char **out, int max) { (void)out, (void)max; return 0; }
int vk_dlss_record(VkCommandBuffer cb, uint64_t fence, const VkTexImage *in, const VkTexImage *out) {
    (void)cb, (void)fence, (void)in, (void)out;
    return 0;
}
void vk_dlss_release(void) {}

#endif
