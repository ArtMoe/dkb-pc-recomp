#include "../runtime/dkb_camera.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>
static GuestFn callbacks[5];
static int registered, has_patch = 1;
int rt_patch_set(const char *name) { assert(!strcmp(name,"mousecamera")); return has_patch ? 0 : -1; }
void *rt_patch_page(void) { return (void *)(uintptr_t)0x7002450000ull; }
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(registered < 5 && addr == 0x7002450200ull + registered*16);
    callbacks[registered++] = fn;
}
static float test_dx, test_dy;
static int test_enabled = 1;
static int64_t test_tick = 100;
int64_t frame_step_world_us(void) { return test_tick; }
int input_mouse_direct(float out[2]) { out[0]=test_dx; out[1]=test_dy; return test_enabled; }
static _Alignas(8) uint8_t follow[0x400], vertical[0x200], service[0x100], options[0x100], timing[0x40], limits[0x40], defaults[0x40];
static void reset(void) {
    memset(&mouse,0,sizeof mouse); memset(follow,0,sizeof follow); memset(vertical,0,sizeof vertical);
    memset(service,0,sizeof service); memset(options,0,sizeof options);
    memset(limits,0,sizeof limits); memset(defaults,0,sizeof defaults);
    PTR(follow,0xb0)=(uintptr_t)service; PTR(follow,0x168)=(uintptr_t)vertical;
    PTR(service,0x60)=(uintptr_t)options; PTR(service,0x10)=(uintptr_t)timing;
    PTR(vertical,0xc8)=(uintptr_t)defaults; PTR(vertical,0xd0)=(uintptr_t)limits;
    FLOAT(timing,4)=1; FLOAT(vertical,0x104)=FLOAT(vertical,0x108)=10;
    test_dx=test_dy=0; test_enabled=1; test_tick=100;
}
static void begin(void) { camera_hook(0,(uintptr_t)follow); }
static void yaw(void) { camera_hook(1,(uintptr_t)follow); }
static void pitch(void) { camera_hook(2,(uintptr_t)vertical); camera_hook(3,(uintptr_t)vertical); }
int main(void) {
    has_patch=0; dkb_camera_init(); assert(registered==0);
    has_patch=1; dkb_camera_init(); assert(registered==5);
    reset(); test_dx=8; test_dy=-4;
    Cpu c={0}; c.x[0]=(uintptr_t)follow; c.x[19]=(uintptr_t)follow;
    Cpu before=c; callbacks[0](&c); callbacks[1](&c); assert(!memcmp(&c,&before,sizeof c));
    c.x[19]=(uintptr_t)vertical; before=c; callbacks[2](&c); callbacks[3](&c);
    assert(!memcmp(&c,&before,sizeof c)); assert(FLOAT(follow,0x204)==-8 && FLOAT(vertical,0x104)==6);
    reset(); test_dx=test_dy=8; begin(); test_tick++; yaw(); pitch();
    assert(FLOAT(follow,0x204)==0 && FLOAT(vertical,0x104)==10);
    reset(); test_dy=8; begin(); camera_hook(PITCH_END,(uintptr_t)vertical);
    assert(FLOAT(vertical,0x104)==10); // no matching pitch begin

    reset(); FLOAT(follow,0x204)=2; FLOAT(follow,0x208)=4; begin(); yaw(); pitch();
    assert(FLOAT(follow,0x204)==2 && FLOAT(follow,0x208)==4); // idle mouse preserves controller/auto camera
    assert(FLOAT(vertical,0x104)==10);
    reset(); test_dx=8; test_dy=-4; FLOAT(follow,0x208)=7; begin(); yaw(); pitch();
    assert(FLOAT(follow,0x204)==-8 && FLOAT(follow,0x208)==0);
    assert(FLOAT(vertical,0x104)==6 && FLOAT(vertical,0x108)==6);
    pitch(); assert(FLOAT(vertical,0x104)==6); // a second call cannot reuse the same mouse distance
    reset(); test_dx=8; test_dy=-4; BYTE(options,8)=BYTE(options,9)=1; begin(); yaw(); pitch();
    assert(FLOAT(follow,0x204)==8 && FLOAT(vertical,0x104)==14);
    reset(); test_dy=1000; begin(); pitch(); assert(FLOAT(vertical,0x104)==85);
    reset(); test_dy=-1000; begin(); pitch(); assert(FLOAT(vertical,0x104)==-70);
    reset(); BYTE(limits,0)=1; FLOAT(limits,4)=-20; FLOAT(limits,8)=30;
    test_dy=1000; begin(); pitch(); assert(FLOAT(vertical,0x104)==30);
    reset(); BYTE(defaults,0x10)=1; FLOAT(defaults,4)=-15; FLOAT(defaults,8)=25;
    test_dy=-1000; begin(); pitch(); assert(FLOAT(vertical,0x104)==-15);
    reset(); test_dx=test_dy=12; test_enabled=0; begin(); yaw(); pitch();
    assert(FLOAT(follow,0x204)==0 && FLOAT(vertical,0x104)==10);
    reset(); test_dx=test_dy=12; FLOAT(timing,4)=0; begin(); yaw(); pitch();
    assert(FLOAT(follow,0x204)==0 && FLOAT(vertical,0x104)==10); // paused game
    // Native transition checks receive activity only from a current, enabled vertical mouse sample.
    reset(); test_dy=0.01f; begin(); c.x[19]=(uintptr_t)vertical; c.v[0]=0;
    before=c; callbacks[4](&c); before.v[0]=0x3f800000u; assert(!memcmp(&c,&before,sizeof c));
    test_tick++; c.v[0]=0; callbacks[4](&c); assert(c.v[0]==0);
    reset(); begin(); callbacks[4](&c); assert(c.v[0]==0); // horizontal/idle input must not wake pitch
    reset(); test_dy=1; test_enabled=0; begin(); callbacks[4](&c); assert(c.v[0]==0);
    puts("PASS: immediate yaw/pitch, idle controller, inversion, native limits, one-time consumption, blocking, pause, stale pitch");
}
