#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <stdlib.h>
static inline int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
#define usleep(us) Sleep((us) / 1000)
#endif
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_text_input_is_active(void) { return 0; }
int bbgpu_mouse_captured(void) { return 1; }
int bbgpu_menu_pointer_mouse_buttons(void) { return 0; }
uint32_t bbgpu_menu_pointer_buttons(uint32_t buttons, uint8_t lx, uint8_t ly) { (void)buttons; (void)lx; (void)ly; return 0; }
void bbgpu_consume_mouse_delta(float *dx, float *dy) { if (dx) *dx = 0.0f; if (dy) *dy = 0.0f; }
int bbgpu_mouse_active_recent(void) { return 0; }
int bbgpu_get_mk_native_mouse(void) { return 1; }
int native_camera_owns_mouse(void) { return 1; }
int bbgpu_get_mk_config(float *sens_x, float *sens_y, int *invert_x, int *invert_y, float *deadzone, float *smoothing) {
    if (sens_x) { *sens_x = 1.0f; }
    if (sens_y) { *sens_y = 1.0f; }
    if (invert_x) { *invert_x = 0; }
    if (invert_y) { *invert_y = 0; }
    if (deadzone) { *deadzone = 0.05f; }
    if (smoothing) { *smoothing = 0.2f; }
    return 1;
}
int bbgpu_get_action_binding(int action) { (void)action; return 0; }
void bbgpu_get_all_action_bindings(int *out_bindings, int count) {
    if (out_bindings) memset(out_bindings, 0, sizeof(int) * count);
}
void bbgpu_set_action_binding(int action, int code) { (void)action; (void)code; }
uint64_t host_monotonic_ns(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ULL) / freq.QuadPart);
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
#ifdef _WIN32
    char temp_dir[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    char path[512];
    snprintf(path, sizeof(path), "%sbbport-pad-test-XXXXXX", temp_dir);
#else
    char path[]="/tmp/bbport-pad-test-XXXXXX";
#endif
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);

    /* Test key_stick last-input priority (SOCD last-win) */
    KeyStick ks = {0};
    uint8_t sx = 128, sy = 128;
    uint64_t t = 1000000;

    /* 1. Hold Left (A) for 10 frames (160ms) */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 1, 0, 0, 0, &sx, &sy); }
    assert(sx < 5 && sy == 128); /* Full Left */

    /* 2. Press Right (D) while still holding Left (A) for 10 frames -> Right was pressed last, so Right wins! */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 1, 1, 0, 0, &sx, &sy); }
    assert(sx > 250 && sy == 128); /* Full Right */

    /* 3. Release Right (D) while still holding Left (A) for 10 frames -> Left resumes! */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 1, 0, 0, 0, &sx, &sy); }
    assert(sx < 5 && sy == 128); /* Resumed Full Left */

    /* 4. Release all -> centers to 128 */
    for (int i = 0; i < 10; ++i) { t += 16000; if (!key_stick(&ks, t, 0, 0, 0, 0, &sx, &sy)) { sx = 128; sy = 128; } }
    assert(sx == 128 && sy == 128);

    /* 5. Vertical: Hold Up (W) for 10 frames */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 0, 0, 1, 0, &sx, &sy); }
    assert(sy < 5 && sx == 128); /* Full Up */

    /* 6. Press Down (S) while holding Up (W) -> Down was pressed last, Down wins! */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 0, 0, 1, 1, &sx, &sy); }
    assert(sy > 250 && sx == 128); /* Full Down */

    /* 7. Release Down (S) while holding Up (W) -> Up resumes! */
    for (int i = 0; i < 10; ++i) { t += 16000; key_stick(&ks, t, 0, 0, 1, 0, &sx, &sy); }
    assert(sy < 5 && sx == 128); /* Resumed Full Up */

    /* 8. Release all -> centers to 128 */
    for (int i = 0; i < 10; ++i) { t += 16000; if (!key_stick(&ks, t, 0, 0, 0, 0, &sx, &sy)) { sx = 128; sy = 128; } }
    assert(sx == 128 && sy == 128);

    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, key_stick SOCD");
}
