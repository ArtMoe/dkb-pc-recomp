// Bananza 1.0.0 manual mouse orbit (recomp/patches-dkb100.txt, mousecamera). Callbacks surround
// CameraPoserFollowLimit's yaw update and CameraAngleVerticalCtrl::exeUserCtrl. The native
// poser still computes the orbit, target and collision; other camera modes keep stick input.
#include <math.h>
#include <stdint.h>
#include "input.h"
#include "host.h"
#include "dispatch.h"

#define FIELD(type, p, offset) (*(type *)((uint8_t *)(uintptr_t)(p) + (offset)))
#define FLOAT(p, offset) FIELD(float, p, offset)
#define PTR(p, offset) FIELD(uint64_t, p, offset)
#define BYTE(p, offset) FIELD(uint8_t, p, offset)

// A camera update's callbacks run on its guest thread. Keep their transient state separate
// from camera work on other host threads; object and game-tick checks reject stale pairs.
static _Thread_local struct {
    uint64_t follow, vertical;
    int64_t tick;
    float yaw, pitch, pitch_before;
    int enabled, yaw_used, pitch_used, pitch_ready;
} mouse;

enum { YAW_BEGIN, YAW_END, PITCH_BEGIN, PITCH_END };

static float pitch_limit(uint64_t vertical, float angle) {
    uint64_t limits = PTR(vertical, 0xd0), defaults = PTR(vertical, 0xc8);
    float low = -70, high = 85;
    if (BYTE(limits, 0)) {
        low = FLOAT(limits, 4); high = FLOAT(limits, 8);
    } else if (BYTE(defaults, 0x10)) {
        low = FLOAT(defaults, 4); high = FLOAT(defaults, 8);
    }
    low = fmaxf(-87.5f, low);
    high = fminf(87.5f, high);
    return fmaxf(low, fminf(high, angle));
}

static void camera_hook(unsigned site, uint64_t obj) {
    if (site == YAW_BEGIN) {
        float delta[2];
        mouse.enabled = input_mouse_direct(delta);
        mouse.follow = obj;
        mouse.vertical = PTR(obj, 0x168);
        mouse.tick = frame_step_world_us();
        mouse.enabled &= FLOAT(PTR(PTR(obj, 0xb0), 0x10), 4) > 0;
        // Use the game's invert settings, shared with its stick camera.
        uint64_t options = PTR(PTR(obj, 0xb0), 0x60);
        // Positive right-stick X becomes a negative orbit angle in the native follow camera.
        mouse.yaw = BYTE(options, 8) ? delta[0] : -delta[0];
        mouse.pitch = BYTE(options, 9) ? -delta[1] : delta[1];
        mouse.yaw_used = mouse.pitch_used = mouse.pitch_ready = 0;
    } else if (site == YAW_END && obj == mouse.follow && mouse.enabled && !mouse.yaw_used &&
               mouse.tick == frame_step_world_us()) {
        mouse.yaw_used = 1;
        if (mouse.yaw != 0) {
            // +0x204 is this update's yaw step; +0x208 is the stick's smoothing accumulator.
            FLOAT(obj, 0x204) = mouse.yaw;
            FLOAT(obj, 0x208) = 0;
            // Manual input cancels the automatic follow turn, as the native stick path does.
            FLOAT(obj, 0x2d8) = 0;
            FLOAT(obj, 0x2fc) = 0;
            BYTE(obj, 0x300) = 0;
        }
    } else if (site == PITCH_BEGIN && obj == mouse.vertical && mouse.tick == frame_step_world_us()) {
        mouse.pitch_before = FLOAT(obj, 0x104);
        mouse.pitch_ready = 1;
    } else if (site == PITCH_END && obj == mouse.vertical && mouse.enabled && mouse.pitch_ready && !mouse.pitch_used &&
               mouse.tick == frame_step_world_us()) {
        mouse.pitch_used = 1;
        if (mouse.pitch != 0) {
            float angle = pitch_limit(obj, mouse.pitch_before + mouse.pitch);
            // Keep the current and target angles together so stick interpolation adds no delay.
            FLOAT(obj, 0x104) = FLOAT(obj, 0x108) = angle;
            FLOAT(obj, 0x130) = 0;
        }
    }
}

static void yaw_begin(Cpu *c) { camera_hook(YAW_BEGIN, c->x[0]); }
static void yaw_end(Cpu *c) { camera_hook(YAW_END, c->x[19]); }
static void pitch_begin(Cpu *c) { camera_hook(PITCH_BEGIN, c->x[19]); }
static void pitch_end(Cpu *c) { camera_hook(PITCH_END, c->x[19]); }

// Interp, SnapEnd and AcceptRequest can wait for vertical stick activity after automatic
// camera adjustments. Report mouse activity to their existing interrupt checks; the game
// chooses the next manual state. Do not force a state or interrupt a scripted/fixed camera.
static void pitch_activity(Cpu *c) {
    if (c->x[19] == mouse.vertical && mouse.enabled && mouse.pitch != 0 &&
        mouse.tick == frame_step_world_us())
        c->v[0] = 0x3f800000u; // s0 = 1.0, an activity signal rather than an angular step
}

void dkb_camera_init(void) {
    if (rt_patch_set("mousecamera") < 0) return;   // another game or generated code predating the patch
    uint64_t page = (uint64_t)(uintptr_t)rt_patch_page();
    rt_register_host(page + 0x200, yaw_begin);
    rt_register_host(page + 0x210, yaw_end);
    rt_register_host(page + 0x220, pitch_begin);
    rt_register_host(page + 0x230, pitch_end);
    rt_register_host(page + 0x240, pitch_activity);
}
