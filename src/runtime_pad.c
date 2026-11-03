/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (when no gamepad is connected):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { return host_monotonic_ns() / 1000u; }
static uint8_t axis(int16_t v) {
    const int deadzone = 3200;
    if (v > -deadzone && v < deadzone) return 128;
    float scaled = (v > 0) ? (float)(v - deadzone) / (32767 - deadzone)
                           : (float)(v + deadzone) / (32768 - deadzone);
    int x = 128 + (int)lroundf(scaled * (scaled > 0 ? 127.0f : 128.0f));
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static uint8_t synth_touch_id = 1;
static uint64_t touch_down_us = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Analog simulation for keyboard movement: smooth acceleration (rise 70ms, fall 45ms),
 * natural angular sweep on direction changes (1100 deg/s), true unit-circle normalization
 * so diagonal movement does not overshoot or jerk, straight movements stay dead-center,
 * and last-input priority (SOCD last-win) on opposing keys so strafing does not dead-stop. */
typedef struct {
    float angle, mag;
    uint64_t last_us;
    uint64_t press_seq;
    uint64_t left_order, right_order, up_order, down_order;
    uint8_t prev_left, prev_right, prev_up, prev_down;
} KeyStick;

static float key_tunable(const char *name, float def) {
    const char *env = getenv(name);
    if (!env || !*env) return def;
    const float v = (float)atof(env);
    return v > 0.0f ? v : def;
}

static int key_stick(KeyStick *st, uint64_t now, int left, int right, int up, int down, uint8_t *x, uint8_t *y) {
    static float rise_ms, fall_ms, turn_dps;
    if (rise_ms == 0.0f) {
        rise_ms = key_tunable("BB_KEY_RISE_MS", 70.0f);
        fall_ms = key_tunable("BB_KEY_FALL_MS", 45.0f);
        turn_dps = key_tunable("BB_KEY_TURN_DPS", 1100.0f);
    }

    const int l_press = left && !st->prev_left;
    const int r_press = right && !st->prev_right;
    const int u_press = up && !st->prev_up;
    const int d_press = down && !st->prev_down;

    if (l_press && r_press) {
        const uint64_t seq = ++st->press_seq;
        st->left_order = seq;
        st->right_order = seq;
    } else if (l_press) {
        st->left_order = ++st->press_seq;
    } else if (r_press) {
        st->right_order = ++st->press_seq;
    }

    if (u_press && d_press) {
        const uint64_t seq = ++st->press_seq;
        st->up_order = seq;
        st->down_order = seq;
    } else if (u_press) {
        st->up_order = ++st->press_seq;
    } else if (d_press) {
        st->down_order = ++st->press_seq;
    }

    st->prev_left = left ? 1 : 0;
    st->prev_right = right ? 1 : 0;
    st->prev_up = up ? 1 : 0;
    st->prev_down = down ? 1 : 0;

    int tx = 0;
    if (left && right) {
        if (st->right_order > st->left_order) tx = 1;
        else if (st->left_order > st->right_order) tx = -1;
        else tx = 0;
    } else if (right) {
        tx = 1;
    } else if (left) {
        tx = -1;
    }

    int ty = 0;
    if (up && down) {
        if (st->down_order > st->up_order) ty = 1;
        else if (st->up_order > st->down_order) ty = -1;
        else ty = 0;
    } else if (down) {
        ty = 1;
    } else if (up) {
        ty = -1;
    }

    float dt = (st->last_us && now > st->last_us) ? (float)(now - st->last_us) * 1e-6f : 0.0f;
    st->last_us = now;
    if (dt > 0.05f) dt = 0.05f;
    const float target_mag = (tx || ty) ? 1.0f : 0.0f;
    if (tx || ty) {
        const float target = atan2f((float)ty, (float)tx);
        if (st->mag < 0.12f) {
            st->angle = target;
        } else {
            float diff = target - st->angle;
            while (diff > (float)M_PI) diff -= 2.0f * (float)M_PI;
            while (diff < -(float)M_PI) diff += 2.0f * (float)M_PI;
            if (fabsf(fabsf(diff) - (float)M_PI) < 0.15f) {
                st->angle = target;
                st->mag = 0.0f;
            } else {
                const float step = turn_dps * ((float)M_PI / 180.0f) * dt;
                st->angle += (diff > step) ? step : ((diff < -step) ? -step : diff);
            }
        }
    }
    const float rate = (target_mag > st->mag) ? (1.0f / (rise_ms * 1e-3f)) : (1.0f / (fall_ms * 1e-3f));
    const float dm = rate * dt;
    st->mag += (target_mag > st->mag) ? ((target_mag - st->mag < dm) ? (target_mag - st->mag) : dm)
                                      : -((st->mag - target_mag < dm) ? (st->mag - target_mag) : dm);
    if (st->mag < 0.003f) {
        st->mag = 0.0f;
        return 0;
    }
    float fx = cosf(st->angle) * st->mag;
    float fy = sinf(st->angle) * st->mag;
    if (tx == 0 && ty != 0 && fabsf(fx) < 0.005f) fx = 0.0f;
    if (ty == 0 && tx != 0 && fabsf(fy) < 0.005f) fy = 0.0f;
    int vx = 128 + (int)lroundf(fx * 127.0f);
    int vy = 128 + (int)lroundf(fy * 127.0f);
    *x = (uint8_t)(vx < 0 ? 0 : vx > 255 ? 255 : vx);
    *y = (uint8_t)(vy < 0 ? 0 : vy > 255 ? 255 : vy);
    return 1;
}

static KeyStick move_stick;

static void touch_click(PadData *d, int right) {
    uint64_t t = now_us();
    if (!touch_down_us) {
        touch_down_us = t;
        if (++synth_touch_id > 127 || synth_touch_id == 0) synth_touch_id = 1;
    }
    d->buttons |= BTN_TOUCHPAD;
    d->touch_count = 1;
    d->touch_held_time = (uint32_t)(t - touch_down_us);
    d->touches[0] = (PadTouch){.x = right ? 1440 : 480, .y = 471, .id = synth_touch_id};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}

enum { CHORD_LMB = 1, CHORD_RMB = 2 };

/* lmb/rmb are the bare buttons: false while a bound Shift chord on that button is held. */
static inline bool is_action_down(int code, const bool *k, bool lmb, bool rmb, bool mmb, bool x1, bool x2, unsigned chords) {
    if (code <= 0) return false;
    if (code == BB_KEY_MOUSE_LEFT) return lmb;
    if (code == BB_KEY_MOUSE_RIGHT) return rmb;
    if (code == BB_KEY_MOUSE_MIDDLE) return mmb;
    if (code == BB_KEY_MOUSE_X1) return x1;
    if (code == BB_KEY_MOUSE_X2) return x2;
    if (code == BB_KEY_CHORD_SHIFT_LMB) return (chords & CHORD_LMB) != 0;
    if (code == BB_KEY_CHORD_SHIFT_RMB) return (chords & CHORD_RMB) != 0;
    if (code < SDL_SCANCODE_COUNT && k) return k[code];
    return false;
}

/* Button prompts (gpu/shim/bbport_button_prompts.cpp): the game draws its key guide with 32x32
 * icons of PlayStation buttons. The port draws the bound key, the mouse button or another pad's
 * button instead while that device was used last. prompt_device: 0 a PlayStation pad (the game's
 * own icons), 1 another pad (its labels), 2 the keyboard and mouse. The generation changes with
 * the device, the pad's type and the bindings; the renderer redraws the icons then. */
static int prompt_device=-1;
static int prompt_pad_type=SDL_GAMEPAD_TYPE_UNKNOWN;
static int prompt_pad_touchpad;
static unsigned prompt_generation=1;
static unsigned prompt_bind_hash=1;

static unsigned prompt_bindings_hash(void) {
    int binds[BB_ACTION_GAMEPLAY_COUNT];
    bbgpu_get_all_action_bindings(binds,BB_ACTION_GAMEPLAY_COUNT);
    unsigned h=2166136261u;
    for (int i=0;i<BB_ACTION_GAMEPLAY_COUNT;++i) h=(h^(unsigned)binds[i])*16777619u;
    return h;
}
static int prompt_playstation(int type) {
    return type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 ||
           type==SDL_GAMEPAD_TYPE_PS5;
}
static int prompt_nintendo(int type) {
    return type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
           type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
           type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
           type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
}
static void prompt_note(SDL_Gamepad *g,int pad_active,int keyboard_active) {
    const int type=g ? (int)SDL_GetGamepadType(g) : (int)SDL_GAMEPAD_TYPE_UNKNOWN;
    const int touchpad=g ? SDL_GetNumGamepadTouchpads(g)>0 : 0;
    const unsigned binds=prompt_bindings_hash();
    const int previous=__atomic_load_n(&prompt_device,__ATOMIC_RELAXED);
    int device=previous;
    if (keyboard_active) device=2;
    else if (g && (pad_active || device<0)) device=prompt_playstation(type) ? 0 : 1;
    else if (device<0) device=2;
    if (device!=previous ||
        (device!=2 && (type!=prompt_pad_type || touchpad!=prompt_pad_touchpad)) ||
        binds!=prompt_bind_hash) {
        __atomic_store_n(&prompt_device,device,__ATOMIC_RELAXED);
        __atomic_store_n(&prompt_pad_type,type,__ATOMIC_RELAXED);
        __atomic_store_n(&prompt_pad_touchpad,touchpad,__ATOMIC_RELAXED);
        prompt_bind_hash=binds;
        __atomic_add_fetch(&prompt_generation,1,__ATOMIC_RELEASE);
    }
}
int runtime_pad_prompt_device(void) {
    return __atomic_load_n(&prompt_device,__ATOMIC_RELAXED);
}
unsigned runtime_pad_prompt_generation(void) {
    return __atomic_load_n(&prompt_generation,__ATOMIC_ACQUIRE);
}
int runtime_pad_mouse_look(void) {
    return bbgpu_get_mk_native_mouse();
}

/* The PS input name an icon stands for -> the action the port binds to it. */
static int prompt_action(const char *name) {
    if (!strcmp(name,"cross")) return BB_ACTION_INTERACT;
    if (!strcmp(name,"circle")) return BB_ACTION_DODGE;
    if (!strcmp(name,"square")) return BB_ACTION_USE_ITEM;
    if (!strcmp(name,"triangle")) return BB_ACTION_HEAL;
    if (!strcmp(name,"l1")) return BB_ACTION_TRANSFORM;
    if (!strcmp(name,"l2")) return BB_ACTION_FIREARM;
    if (!strcmp(name,"r1")) return BB_ACTION_ATTACK;
    if (!strcmp(name,"r2")) return BB_ACTION_HEAVY_ATTACK;
    if (!strcmp(name,"r3")) return BB_ACTION_LOCK_ON;
    if (!strcmp(name,"options")) return BB_ACTION_MENU;
    if (!strcmp(name,"touchpad")) return BB_ACTION_GESTURES;
    if (!strcmp(name,"touchpad_right")) return BB_ACTION_PERSONAL_EFFECTS;
    if (!strcmp(name,"up")) return BB_ACTION_DPAD_UP;
    if (!strcmp(name,"down")) return BB_ACTION_DPAD_DOWN;
    if (!strcmp(name,"left")) return BB_ACTION_DPAD_LEFT;
    if (!strcmp(name,"right")) return BB_ACTION_DPAD_RIGHT;
    if (!strcmp(name,"move_up")) return BB_ACTION_MOVE_FORWARD;
    if (!strcmp(name,"move_down")) return BB_ACTION_MOVE_BACKWARD;
    if (!strcmp(name,"move_left")) return BB_ACTION_MOVE_LEFT;
    if (!strcmp(name,"move_right")) return BB_ACTION_MOVE_RIGHT;
    return -1; /* l3: unbound; look_*: the mouse (runtime_pad_mouse_look) */
}
static const char *prompt_mouse_label(int code) {
    switch (code) {
    case BB_KEY_MOUSE_LEFT: return "Left";
    case BB_KEY_MOUSE_RIGHT: return "Right";
    case BB_KEY_MOUSE_MIDDLE: return "Middle";
    case BB_KEY_MOUSE_X1: return "X1";
    case BB_KEY_MOUSE_X2: return "X2";
    default: return NULL;
    }
}
/* A key cap's label: the active layout's character for letter/punctuation keys,
 * otherwise SDL's physical name shortened for the common long ones. */
static void prompt_key_label(int code,char *out,int size) {
    static const struct { const char *from, *to; } names[]={
        {"Return","Enter"}, {"Escape","Esc"}, {"Backspace","Bksp"},
        {"PageUp","PgUp"}, {"PageDown","PgDn"}, {"CapsLock","Caps"},
        {"Left Shift","Shift"}, {"Right Shift","RShift"},
        {"Left Ctrl","Ctrl"}, {"Right Ctrl","RCtrl"},
        {"Left Alt","Alt"}, {"Right Alt","RAlt"},
        {"Left GUI","Win"}, {"Right GUI","RWin"},
        {"ScrollLock","Scrl"}, {"NumLockClear","Num"},
        {"Application","Menu"}, {"PrintScreen","PrtSc"},
    };
    const char *name=SDL_GetScancodeName((SDL_Scancode)code);
    if (!name || !*name) { snprintf(out,(size_t)size,"Key %d",code); return; }
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        if (!strcmp(name,names[i].from)) { snprintf(out,(size_t)size,"%s",names[i].to); return; }
    }
    /* The game binds SDL scancodes, which are named for physical US keys.  Show
     * the character printed on the active AZERTY/QWERTY key instead.  Digits stay
     * as 1..0: their unshifted AZERTY glyphs (&, é, ...) make poor control caps. */
    const int localized=(code>=SDL_SCANCODE_A && code<=SDL_SCANCODE_Z) ||
                        (code>=SDL_SCANCODE_MINUS && code<=SDL_SCANCODE_SLASH);
    if (localized) {
        const char *local=SDL_GetKeyName(
            SDL_GetKeyFromScancode((SDL_Scancode)code,SDL_KMOD_NONE,false));
        if (local && *local && strcmp(local,"Unknown")) name=local;
    }
    snprintf(out,(size_t)size,"%s",name);
}

/* The label of a PS input for the device in use: 'K' a key (a key cap), 'M' a mouse button,
 * 'F' a face button, 'S' a shoulder/trigger, 'C' a round pad button; 0: the game's own icon. */
int runtime_pad_prompt(const char *name,int prefer_mouse,char *label,int size) {
    (void)prefer_mouse;
    if (!name || !label || size<=0) return 0;
    label[0]=0;
    const int device=__atomic_load_n(&prompt_device,__ATOMIC_RELAXED);
    if (device==2) {
        const int action=prompt_action(name);
        if (action<0) return 0;
        const int code=bbgpu_get_action_binding(action);
        if (code<=0) {
            /* Cross also answers Enter, whatever it is bound to. */
            if (!strcmp(name,"cross")) { snprintf(label,(size_t)size,"Enter"); return 'K'; }
            return 0;
        }
        if (code==BB_KEY_CHORD_SHIFT_LMB || code==BB_KEY_CHORD_SHIFT_RMB) {
            snprintf(label,(size_t)size,"\xe2\x87\xa7%s",
                     code==BB_KEY_CHORD_SHIFT_LMB ? "Left" : "Right");
            return 'M';
        }
        const char *mouse=prompt_mouse_label(code);
        if (mouse) { snprintf(label,(size_t)size,"%s",mouse); return 'M'; }
        if (code<SDL_SCANCODE_COUNT) { prompt_key_label(code,label,size); return 'K'; }
        return 0;
    }
    if (device==1) {
        const int type=__atomic_load_n(&prompt_pad_type,__ATOMIC_RELAXED);
        const int touchpad=__atomic_load_n(&prompt_pad_touchpad,__ATOMIC_RELAXED);
        const int nin=prompt_nintendo(type);
        const char *face=NULL, *shoulder=NULL;
        if (!strcmp(name,"cross")) face=nin ? "B" : "A";
        else if (!strcmp(name,"circle")) face=nin ? "A" : "B";
        else if (!strcmp(name,"square")) face=nin ? "Y" : "X";
        else if (!strcmp(name,"triangle")) face=nin ? "X" : "Y";
        if (face) { snprintf(label,(size_t)size,"%s",face); return 'F'; }
        if (!strcmp(name,"l1")) shoulder=nin ? "L" : "LB";
        else if (!strcmp(name,"r1")) shoulder=nin ? "R" : "RB";
        else if (!strcmp(name,"l2")) shoulder=nin ? "ZL" : "LT";
        else if (!strcmp(name,"r2")) shoulder=nin ? "ZR" : "RT";
        if (shoulder) { snprintf(label,(size_t)size,"%s",shoulder); return 'S'; }
        if (!strcmp(name,"l3")) { snprintf(label,(size_t)size,"LS"); return 'C'; }
        if (!strcmp(name,"r3")) { snprintf(label,(size_t)size,"RS"); return 'C'; }
        if (!strcmp(name,"options")) { snprintf(label,(size_t)size,"%s",nin ? "+" : "Menu"); return 'C'; }
        if (!strcmp(name,"touchpad") || !strcmp(name,"touchpad_right")) {
            snprintf(label,(size_t)size,"%s",touchpad ? "Pad" : (nin ? "-" : "View"));
            return 'C';
        }
        if (!strcmp(name,"move_up") || !strcmp(name,"move_down") ||
            !strcmp(name,"move_left") || !strcmp(name,"move_right")) {
            snprintf(label,(size_t)size,"LS"); return 'C';
        }
        if (!strcmp(name,"look_up") || !strcmp(name,"look_down") ||
            !strcmp(name,"look_left") || !strcmp(name,"look_right")) {
            snprintf(label,(size_t)size,"RS"); return 'C';
        }
        return 0; /* the d-pad's icons fit any pad */
    }
    return 0;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (bbgpu_overlay_captures_input() || bbgpu_text_input_is_active()) return; /* settings menu open or typing: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) if (SDL_GetGamepadButton(g,map[i].sdl)) d->buttons|=map[i].ps;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    // The pad was used this sample (the keyboard, below, may still win as the last device).
    const int pad_active = g && (d->buttons || d->l2 > 30 || d->r2 > 30 || d->touch_count ||
                                 d->left_x < 80 || d->left_x > 176 ||
                                 d->left_y < 80 || d->left_y > 176 ||
                                 d->right_x < 80 || d->right_x > 176 ||
                                 d->right_y < 80 || d->right_y > 176);
    if (!k) return;

    // Souls PC Mouse & Keyboard integration
    float sens_x = 1.0f, sens_y = 1.0f;
    int inv_x = 0, inv_y = 0;
    const int mk_enabled = bbgpu_get_mk_config(&sens_x, &sens_y, &inv_x, &inv_y, NULL, NULL);

    // Mouse Buttons (only active while mouse is captured in relative mode, or the menu pointer has it)
    const int mouse_is_captured = bbgpu_mouse_captured();
    // The buttons the menu pointer takes (1 left, 2 right) choose and go back instead of their bindings.
    const int pointer_owns = mouse_is_captured ? 0 : bbgpu_menu_pointer_mouse_buttons();
    float mouse_x_pos = 0.0f, mouse_y_pos = 0.0f;
    const SDL_MouseButtonFlags mouse_buttons = (mouse_is_captured || pointer_owns) ? SDL_GetMouseState(&mouse_x_pos, &mouse_y_pos) : 0;
    const bool lmb_raw = (mouse_buttons & SDL_BUTTON_LMASK) != 0 && !(pointer_owns & 1);
    const bool rmb_raw = (mouse_buttons & SDL_BUTTON_RMASK) != 0 && !(pointer_owns & 2);
    const bool mmb = (mouse_buttons & SDL_BUTTON_MMASK) != 0;
    const bool x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0;
    const bool x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0;

    const bool lshift = k && (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]);

    int binds[BB_ACTION_COUNT];
    bbgpu_get_all_action_bindings(binds, BB_ACTION_COUNT);

    // A held Shift chord owns its mouse button so the bare-button action doesn't fire with it.
    unsigned chords = 0;
    for (int i = 0; i < BB_ACTION_GAMEPLAY_COUNT; ++i) {
        if (binds[i] == BB_KEY_CHORD_SHIFT_LMB && lshift && lmb_raw) chords |= CHORD_LMB;
        if (binds[i] == BB_KEY_CHORD_SHIFT_RMB && lshift && rmb_raw) chords |= CHORD_RMB;
    }
    const bool lmb = lmb_raw && !(chords & CHORD_LMB);
    const bool rmb = rmb_raw && !(chords & CHORD_RMB);

    // Button prompts: the device used last. A bound key, mouse button or the mouse's look
    // movement means the keyboard and mouse; Enter and the arrow keys always answer too. The
    // look delta is kept for the camera below.
    const int mouse_look_owns = mk_enabled && mouse_is_captured && !native_camera_owns_mouse();
    float prompt_mouse_dx = 0.0f, prompt_mouse_dy = 0.0f;
    if (mouse_look_owns) bbgpu_consume_mouse_delta(&prompt_mouse_dx, &prompt_mouse_dy);
    // Any recent mouse input switches the prompts, even when the mouse is not captured (a pad
    // user moving the mouse): the camera delta alone is consumed by the native camera hook and
    // is not accumulated at all while the cursor is free.
    int keyboard_active = bbgpu_mouse_active_recent() ||
                          (mouse_is_captured &&
                           (lmb_raw || rmb_raw || mmb || x1 || x2 ||
                            prompt_mouse_dx * prompt_mouse_dx + prompt_mouse_dy * prompt_mouse_dy > 9.0f));
    for (int i = 0; i < BB_ACTION_GAMEPLAY_COUNT && !keyboard_active; ++i) {
        const int c = binds[i];
        if (c > 0 && c < SDL_SCANCODE_COUNT && k[c]) keyboard_active = 1;
    }
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] ||
        k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN] ||
        k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_RIGHT]) {
        keyboard_active = 1;
    }
    prompt_note(g, pad_active, keyboard_active);

    if (mk_enabled) {
        // Combat actions
        if (is_action_down(binds[BB_ACTION_ATTACK], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_R1;
        if (is_action_down(binds[BB_ACTION_HEAVY_ATTACK], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_R2;
        if (is_action_down(binds[BB_ACTION_FIREARM], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_L2;
        if (is_action_down(binds[BB_ACTION_TRANSFORM], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_L1;
        if (is_action_down(binds[BB_ACTION_LOCK_ON], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_R3;

        // Interaction & quick item actions
        if (is_action_down(binds[BB_ACTION_DODGE], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_CIRCLE;
        if (is_action_down(binds[BB_ACTION_INTERACT], k, lmb, rmb, mmb, x1, x2, chords) ||
            (k && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]))) d->buttons |= BTN_CROSS;
        if (is_action_down(binds[BB_ACTION_USE_ITEM], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_SQUARE;
        if (is_action_down(binds[BB_ACTION_HEAL], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_TRIANGLE;
        if (is_action_down(binds[BB_ACTION_GESTURES], k, lmb, rmb, mmb, x1, x2, chords)) touch_click(d, 0);
        if (is_action_down(binds[BB_ACTION_PERSONAL_EFFECTS], k, lmb, rmb, mmb, x1, x2, chords)) touch_click(d, 1);
        if (is_action_down(binds[BB_ACTION_MENU], k, lmb, rmb, mmb, x1, x2, chords)) d->buttons |= BTN_OPTIONS;

        // D-Pad Quick Selection (with arrow keys fallback)
        if (is_action_down(binds[BB_ACTION_DPAD_UP], k, lmb, rmb, mmb, x1, x2, chords) || (k && k[SDL_SCANCODE_UP])) d->buttons |= BTN_UP;
        if (is_action_down(binds[BB_ACTION_DPAD_DOWN], k, lmb, rmb, mmb, x1, x2, chords) || (k && k[SDL_SCANCODE_DOWN])) d->buttons |= BTN_DOWN;
        if (is_action_down(binds[BB_ACTION_DPAD_LEFT], k, lmb, rmb, mmb, x1, x2, chords) || (k && k[SDL_SCANCODE_LEFT])) d->buttons |= BTN_LEFT;
        if (is_action_down(binds[BB_ACTION_DPAD_RIGHT], k, lmb, rmb, mmb, x1, x2, chords) || (k && k[SDL_SCANCODE_RIGHT])) d->buttons |= BTN_RIGHT;

        // Movement (Left Stick); the walk key holds the stick under the engine's run threshold
        const int walk = is_action_down(binds[BB_ACTION_WALK], k, lmb, rmb, mmb, x1, x2, chords);
        const int move_l = is_action_down(binds[BB_ACTION_MOVE_LEFT], k, lmb, rmb, mmb, x1, x2, chords);
        const int move_r = is_action_down(binds[BB_ACTION_MOVE_RIGHT], k, lmb, rmb, mmb, x1, x2, chords);
        const int move_u = is_action_down(binds[BB_ACTION_MOVE_FORWARD], k, lmb, rmb, mmb, x1, x2, chords);
        const int move_d = is_action_down(binds[BB_ACTION_MOVE_BACKWARD], k, lmb, rmb, mmb, x1, x2, chords);
        if (key_stick(&move_stick, d->timestamp, move_l, move_r, move_u, move_d, &d->left_x, &d->left_y)) {
            if (walk) {
                d->left_x = (uint8_t)(128 + ((int)d->left_x - 128) * 2 / 5);
                d->left_y = (uint8_t)(128 + ((int)d->left_y - 128) * 2 / 5);
            }
        } else if (walk) {
            d->left_x = (uint8_t)(128 + ((int)d->left_x - 128) * 2 / 5);
            d->left_y = (uint8_t)(128 + ((int)d->left_y - 128) * 2 / 5);
        }





        // Mouse as right stick whenever the native camera hook doesn't own it (lock-on target switching):
        // each delta is an impulse that decays over LOOK_TAU_US.
        #define LOOK_TAU_US 60000.0f
        static float look_x = 0.0f, look_y = 0.0f;
        static uint64_t look_last_us = 0;
        const float dt = (look_last_us && d->timestamp > look_last_us) ? (float)(d->timestamp - look_last_us) : 0.0f;
        look_last_us = d->timestamp;
        if (!mouse_look_owns) {
            look_x = look_y = 0.0f;
        } else {
            const float mouse_dx = prompt_mouse_dx, mouse_dy = prompt_mouse_dy;
            const float decay = dt >= LOOK_TAU_US ? 0.0f : 1.0f - dt / LOOK_TAU_US;
            const float gain = 127.0f / (1000.0f * LOOK_TAU_US / 1e6f);
            look_x = look_x * decay + mouse_dx * gain * (inv_x ? -sens_x : sens_x);
            look_y = look_y * decay + mouse_dy * gain * (inv_y ? -sens_y : sens_y);
            if (look_x < -127.0f) look_x = -127.0f; else if (look_x > 127.0f) look_x = 127.0f;
            if (look_y < -127.0f) look_y = -127.0f; else if (look_y > 127.0f) look_y = 127.0f;
            if (fabsf(look_x) >= 0.5f) d->right_x = (uint8_t)(128 + (int)look_x);
            if (fabsf(look_y) >= 0.5f) d->right_y = (uint8_t)(128 + (int)look_y);
        }
    } else {
        // Fallback layout when M&K mode is disabled
        static const struct { SDL_Scancode key; uint32_t ps; } keys[]={
            {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS}, {SDL_SCANCODE_KP_ENTER,BTN_CROSS},
            {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_ESCAPE,BTN_CIRCLE},
            {SDL_SCANCODE_E,BTN_SQUARE}, {SDL_SCANCODE_Q,BTN_TRIANGLE},
            {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
            {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2},
            {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
            {SDL_SCANCODE_F1,BTN_OPTIONS}, {SDL_SCANCODE_O,BTN_OPTIONS},
            {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN},
            {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) if (k[keys[i].key]) d->buttons|=keys[i].ps;
        if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
        if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
        const int key_l = k && k[SDL_SCANCODE_A];
        const int key_r = k && k[SDL_SCANCODE_D];
        const int key_u = k && k[SDL_SCANCODE_W];
        const int key_d = k && k[SDL_SCANCODE_S];
        key_stick(&move_stick, d->timestamp, key_l, key_r, key_u, key_d, &d->left_x, &d->left_y);




        if (k[SDL_SCANCODE_I]) d->right_y = 0;
        else if (k[SDL_SCANCODE_K]) d->right_y = 255;
        if (k[SDL_SCANCODE_J]) d->right_x = 0;
        else if (k[SDL_SCANCODE_L]) d->right_x = 255;
    }

    if (d->buttons & BTN_L2) if (d->l2 < 255) d->l2=255;
    if (d->buttons & BTN_R2) if (d->r2 < 255) d->r2=255;
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check;
#ifndef _WIN32
    static struct timespec mtime;
#endif
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
#ifdef _WIN32
    static FILETIME last_ft;
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetFileTime(hFile, NULL, NULL, &ft);
    CloseHandle(hFile);
    if (ft.dwLowDateTime == last_ft.dwLowDateTime && ft.dwHighDateTime == last_ft.dwHighDateTime) return;
    last_ft = ft;
#else
    if (stat(path,&st)!=0) return;
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input() || bbgpu_text_input_is_active()) return;
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (!d->touch_count) touch_down_us = 0;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    d->buttons|=bbgpu_menu_pointer_buttons(d->buttons,d->left_x,d->left_y);
}

static ABI int32_t pad_init(void) { host_lock(&lock); initialized=1; host_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    host_lock(&lock);
    int already=opened; opened=1;
    host_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    host_lock(&lock);
    sample(data); ++reads;
    host_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    host_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    host_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
