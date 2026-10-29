/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
int bbgpu_text_input_is_active(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
/* Flips and submissions query for heartbeat / diagnostics. */
uint64_t bbgpu_get_flip_count(void);
uint64_t bbgpu_get_submit_count(void);
/* Dumps host GPU / presenter thread states and counters to a log file. */
void bbgpu_dump_host_threads_hang(void *file_handle);
/* Computes average FPS and p95/p99 frame times in milliseconds over recent flips. */
void bbgpu_get_frametime_percentiles(double *avg_fps, double *p95_ms, double *p99_ms);
/* Dumps GPU breadcrumbs state to log/crash files. */
void bbgpu_dump_breadcrumbs(void *file_handle);
/* Retrieves GPU device name, driver version, and Vulkan API version. */
void bbgpu_get_device_info(char *vendor_buf, size_t vendor_sz,
                           char *model_buf, size_t model_sz,
                           char *driver_buf, size_t driver_sz,
                           char *api_buf, size_t api_sz);
/* Consumes accumulated mouse motion delta (pixels) since last call. */
void bbgpu_consume_mouse_delta(float *dx, float *dy);
/* Returns 1 if mouse look is actively capturing the cursor in relative mode. */
int bbgpu_mouse_captured(void);
/* Returns 1 if the mouse produced input (motion, button or wheel) within the last 250 ms. */
int bbgpu_mouse_active_recent(void);
/* Explicitly requests or releases mouse capture. */
void bbgpu_set_mouse_capture(int capture);
/* Returns 1 if M&K is enabled, and retrieves sensitivity and invert flags. */
int bbgpu_get_mk_config(float *sens_x, float *sens_y, int *invert_x, int *invert_y, float *deadzone, float *smoothing);
/* Returns 1 if native direct mouse camera rotation is enabled. */
int bbgpu_get_mk_native_mouse(void);
/* Live engine clock (probe.c hook): mode 1 when live FPS patching is requested, the target
 * rate (0 = uncapped) and the physics max-dt frequency. Relaxed atomic reads only. */
void bbgpu_get_live_clock(int *mode, int *target_fps, int *max_dt_hz);
/* The probe reports whether the eboot signature matched and the hook is installed. */
void bbgpu_set_live_clock_supported(int supported);
/* Per-frame sample for the overlay diagnostic line (dt in seconds, target fps, 0 = uncapped). */
void bbgpu_note_live_clock(float dt, int target_fps);
/* Hooks the game's menu list update for the mouse pointer; before the image is protected. */
int bbgpu_menu_pointer_install(unsigned char *image, uint64_t size);
/* Notes the pad's own state and returns the buttons the menu pointer presses this sample. */
uint32_t bbgpu_menu_pointer_buttons(uint32_t buttons, uint8_t lx, uint8_t ly);
/* The mouse buttons the menu pointer takes while it has the mouse: 1 left (choose), 2 right (back). */
int bbgpu_menu_pointer_mouse_buttons(void);

enum BbCustomKey {
    BB_KEY_NONE = 0,
    BB_KEY_MOUSE_LEFT = 601,
    BB_KEY_MOUSE_RIGHT = 602,
    BB_KEY_MOUSE_MIDDLE = 603,
    BB_KEY_MOUSE_X1 = 604,
    BB_KEY_MOUSE_X2 = 605,
    BB_KEY_CHORD_SHIFT_LMB = 606,
    BB_KEY_CHORD_SHIFT_RMB = 607,
};

enum BbAction {
    BB_ACTION_ATTACK = 0,         /* R1 (Light Attack) */
    BB_ACTION_HEAVY_ATTACK,       /* R2 (Heavy / Charged Attack) */
    BB_ACTION_FIREARM,            /* L2 (Gun / Secondary) */
    BB_ACTION_TRANSFORM,          /* L1 (Trick Weapon Transform) */
    BB_ACTION_LOCK_ON,            /* R3 (Lock Target) */
    BB_ACTION_MOVE_FORWARD,       /* Left Stick Up */
    BB_ACTION_MOVE_BACKWARD,      /* Left Stick Down */
    BB_ACTION_MOVE_LEFT,          /* Left Stick Left */
    BB_ACTION_MOVE_RIGHT,         /* Left Stick Right */
    BB_ACTION_DODGE,              /* Circle (Roll / Sprint / Jump while sprinting) */
    BB_ACTION_INTERACT,           /* Cross (Interact / Pick Up / Talk) */
    BB_ACTION_USE_ITEM,           /* Square (Use Quick Item) */
    BB_ACTION_HEAL,               /* Triangle (Heal / Blood Vial) */
    BB_ACTION_DPAD_UP,            /* D-Pad Up (Blood Bullets) */
    BB_ACTION_DPAD_DOWN,          /* D-Pad Down (Switch Item) */
    BB_ACTION_DPAD_LEFT,          /* D-Pad Left (Switch Left Weapon) */
    BB_ACTION_DPAD_RIGHT,         /* D-Pad Right (Switch Right Weapon) */
    BB_ACTION_GESTURES,           /* Touchpad Left (Gestures Menu) */
    BB_ACTION_PERSONAL_EFFECTS,   /* Touchpad Right (Personal Effects) */
    BB_ACTION_MENU,               /* Options (System Menu) */
    BB_ACTION_WALK,               /* Hold: partial left stick deflection */
    /* End of the gameplay actions. runtime_pad.c only maps these to the virtual pad. */
    BB_ACTION_GAMEPLAY_COUNT,
    /* UI hotkeys: configurable from the launcher and the in-game keybind list. */
    BB_ACTION_OVERLAY_TOGGLE = BB_ACTION_GAMEPLAY_COUNT, /* Settings / debug overlay */
    BB_ACTION_HUD_TOGGLE,         /* Performance HUD */
    BB_ACTION_CHEAT_TOGGLE,       /* In-game cheat menu */
    BB_ACTION_MOUSE_CAPTURE_TOGGLE, /* Capture / release the mouse */
    BB_ACTION_COUNT
};

/* Action keybinding queries */
int bbgpu_get_action_binding(int action);
void bbgpu_get_all_action_bindings(int *out_bindings, int count);
void bbgpu_set_action_binding(int action, int code);

/* Shader recipes & cache rebuilds (Phase 2) */
int bbgpu_rebuild_shader_caches(void);
int bbgpu_rebuild_shader_cache(const char *dir);
int bbgpu_export_shader_recipes(const char *cache_dir, const char *game_dir, const char *out_path);
int bbgpu_import_shader_recipes(const char *cache_dir, const char *pack, const char *game_dir,
                                unsigned long long motion_params, unsigned long long motion_positions);
int bbgpu_merge_shader_metas(const char *cache_dir, const char *pack, const char *game_dir,
                             const char *meta_dir, const char *out_pack, const char *work_dir, int jobs);
#ifdef __cplusplus
}
#endif
#endif
