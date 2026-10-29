/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include "win32_compat.h"
#include "win32_exception.h"
/* Instruction decoding for the non-BMI1 LZCNT sweep and crash-dump RIP annotation. Zydis is
 * already a link dependency of bbgpu (Zydis::Zydis is PUBLIC), so no new runtime dependency. */
#include <Zydis/Zydis.h>
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static uint64_t image_size;
/* Diagnostics must use the real loaded image extent (all linked modules included), not the
 * eboot-only size from an older log. 0 until the image is mapped. */
void runtime_guest_image_range(uint64_t *base, uint64_t *size) {
    if (base) *base = (uint64_t)(uintptr_t)image;
    if (size) *size = image_size;
}
/* The image range is published before relocations/startup patches/module initializers run.
 * A live patcher must not write to the image until those are complete. */
int runtime_guest_image_ready(void) {
    return entered_game;
}
static int is_guest_address(uintptr_t addr) {
    return image && image_size && addr >= (uintptr_t)image && addr < (uintptr_t)image + image_size;
}

static char s_game_title_id[16] = "";
static char s_game_app_ver[16] = "";

static int is_supported_bloodborne_title(const char *title_id) {
    if (!title_id || !title_id[0]) return 1;
    return strcmp(title_id, "CUSA03173") == 0 ||
           strcmp(title_id, "CUSA00207") == 0 ||
           strcmp(title_id, "CUSA03023") == 0 ||
           strcmp(title_id, "CUSA00900") == 0;
}

static int is_cusa03173_v109(void) {
    if (!image || image_size < 0x53e4570) return 0;
    if (*(const int64_t *)(image + 0x53e4568) != -1840) return 0;
    if (s_game_title_id[0] && !is_supported_bloodborne_title(s_game_title_id)) return 0;
    if (s_game_app_ver[0] && strcmp(s_game_app_ver, "01.09") != 0 && strcmp(s_game_app_ver, "01.00") != 0) return 0;
    return 1;
}
static void **resolved_targets;
static uint64_t *import_call_counts;
static uint8_t *is_unbound;
static FILE *g_import_file = NULL;
static volatile uint64_t g_total_import_calls = 0;
static volatile uint64_t g_import_file_lines = 0;
static int g_bb_trace = 0;

typedef struct {
    uint32_t index;
    uint64_t args[6];
} MOMainCallRecord;

#define MO_RING_CAP 64
static MOMainCallRecord mo_recent_calls[MO_RING_CAP];
static uint32_t mo_recent_count = 0;
static uint64_t mo_total_import_count = 0;
static uint64_t *mo_call_counts = NULL;

static volatile uint64_t g_stat_total_seconds = 0;
static volatile uint64_t g_stat_zero_flip_seconds = 0;
static volatile uint64_t g_stat_longest_freeze_s = 0;
static volatile uint64_t g_stat_current_freeze_s = 0;

/* Native direct mouse camera hook & watchdog */
static volatile int g_native_camera_hook_installed = 0;
static volatile int g_native_camera_disabled = 0;
static volatile uint64_t g_native_cam_fire_count = 0;

int native_camera_hook_is_installed(void) {
    return g_native_camera_hook_installed;
}

uint64_t native_camera_get_fire_count(void) {
    return g_native_cam_fire_count;
}

void native_camera_disable_with_log(const char *reason) {
    if (!g_native_camera_disabled) {
        g_native_camera_disabled = 1;
        fprintf(stderr, "[NATIVE MOUSE WATCHDOG] %s\n", reason ? reason : "unknown reason");
        fflush(stderr);
    }
}

int native_camera_is_active(void) {
    return g_native_camera_hook_installed && !g_native_camera_disabled && bbgpu_get_mk_native_mouse();
}

static volatile int g_native_cam_locked_on = 0;

int native_camera_owns_mouse(void) {
    return native_camera_is_active() && !g_native_cam_locked_on;
}

extern void bbgpu_get_mk_tuning(float *responsiveness, float *acceleration);
void (ABI *orig_cam_update)(void *cam, void *r14, void *r15) = NULL;
/* Aim camera (monocular): the camera manager runs it instead of the follow camera in modes 1 and 2. */
void (ABI *orig_aim_cam_update)(void *cam, void *r14) = NULL;

/* Turn speeds (rad/s) for stick input below full tilt: pitch base/max, yaw base/max. */
static const unsigned k_cam_speed_offs[4] = { 0x20c, 0x214, 0x210, 0x218 };
static uintptr_t g_boosted_cam;
static float g_saved_speed[4];

/* The pad leaves the stick buffer untouched in some states (loading), so our last offset is removed if still there. */
static float *g_stick_buf;
static float g_stick_added[2], g_stick_left[2];

/* Mouse smoothing (EMA) state shared by the follow and aim cameras. */
static float s_smooth_dx, s_smooth_dy;

/* Right-stick buffer the camera update reads (rsi_ctx->0x58->0x280, or ->0x80, + 0x40): x = pitch, y = yaw. */
static float *camera_stick_buf(void *rsi_ctx) {
    if (!rsi_ctx || (uintptr_t)rsi_ctx <= 0x10000) return NULL;
    uintptr_t rcx_ptr = *(uintptr_t*)((uintptr_t)rsi_ctx + 0x58);
    if (rcx_ptr <= 0x10000) return NULL;
    uintptr_t rax_val = *(uintptr_t*)(rcx_ptr + 0x280);
    if (!rax_val) rax_val = *(uintptr_t*)(rcx_ptr + 0x80);
    if (rax_val <= 0x10000) return NULL;
    return (float*)(rax_val + 0x40);
}

static void remove_stick_offset(float *stick) {
    if (stick && stick == g_stick_buf && stick[0] == g_stick_left[0] && stick[1] == g_stick_left[1]) {
        stick[0] -= g_stick_added[0];
        stick[1] -= g_stick_added[1];
    }
    g_stick_buf = NULL;
}

static int mouse_look_delta(float delta[2]);
static void follow_cam_apply_delta(uintptr_t cam_addr, float *stick, float dt, const float delta[2]);

void host_native_mouse_inject(void *cam, void *rsi_ctx, float dt) {
    g_native_cam_fire_count++;
    uintptr_t expected_orig = (uintptr_t)image + 0x143ac60;
    if ((uintptr_t)orig_cam_update < (uintptr_t)image ||
        (uintptr_t)orig_cam_update >= (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
        static int s_logged_bad_orig = 0;
        if (!s_logged_bad_orig) {
            s_logged_bad_orig = 1;
            fprintf(stderr, "[NATIVE MOUSE GUARD] Corrupted orig_cam_update pointer (%p), resetting to %p\n",
                    (void*)orig_cam_update, (void*)expected_orig);
            fflush(stderr);
        }
        orig_cam_update = (void (ABI *)(void*, void*, void*))expected_orig;
    }
    uintptr_t cam_addr = (uintptr_t)cam;
    if (cam_addr < 0x10000) return;
    if (g_boosted_cam == cam_addr)
        for (int i = 0; i < 4; ++i) *(float*)(cam_addr + k_cam_speed_offs[i]) = g_saved_speed[i];
    g_boosted_cam = 0;
    float *stick = camera_stick_buf(rsi_ctx);
    remove_stick_offset(stick);
    if (!native_camera_is_active()) return;

    const uint8_t lock_on_byte = *(volatile uint8_t*)(cam_addr + 0x260);
    const int captured = bbgpu_mouse_captured();

    /* Diagnostic log once every 120 camera updates (approx 2s at 60 FPS) */
    static uint64_t s_diag_counter = 0;
    if ((s_diag_counter++ % 120) == 0) {
        printf("[NATIVE MOUSE DIAG] cam=%p pitch=%.3f yaw=%.3f lock_on=%u captured=%d\n",
               cam, *(float*)(cam_addr + 0x140), *(float*)(cam_addr + 0x144), lock_on_byte, captured);
        fflush(stdout);
    }

    /* While locked on the pad turns mouse flicks into right-stick input (target switching). */
    static int s_was_locked = 0;
    g_native_cam_locked_on = lock_on_byte != 0;
    if (lock_on_byte != 0) s_was_locked = 1;
    if (lock_on_byte != 0 || !captured) {
        return;
    }
    if (s_was_locked) {
        s_was_locked = 0;
        s_smooth_dx = s_smooth_dy = 0.0f;
        bbgpu_consume_mouse_delta(NULL, NULL);
    }

    float delta[2];
    if (mouse_look_delta(delta)) follow_cam_apply_delta(cam_addr, stick, dt, delta);
}

/* Consumes the pending mouse motion as camera {pitch, yaw} deltas in radians; 0 when there is none. */
static int mouse_look_delta(float delta[2]) {
    float sens_x = 1.0f, sens_y = 1.0f, dz = 0.0f, sm = 0.0f;
    int inv_x = 0, inv_y = 0;
    const int mk_enabled = bbgpu_get_mk_config(&sens_x, &sens_y, &inv_x, &inv_y, &dz, &sm);
    if (!mk_enabled) return 0;

    float responsiveness = 1.0f, acceleration = 1.0f;
    bbgpu_get_mk_tuning(&responsiveness, &acceleration);

    float dx = 0.0f, dy = 0.0f;
    bbgpu_consume_mouse_delta(&dx, &dy);

    /* Subtle mouse acceleration curve if configured (> 1.0) */
    if (acceleration > 1.0f) {
        const float mag = sqrtf(dx * dx + dy * dy);
        if (mag > 1.0f) {
            const float factor = 1.0f + (acceleration - 1.0f) * (mag / (mag + 30.0f));
            dx *= factor;
            dy *= factor;
        }
    }

    /* Mouse smoothing (EMA filter): 0.0 = raw 1:1, higher = smoothed */
    const float s_factor = (sm < 0.0f) ? 0.0f : (sm > 0.85f ? 0.85f : sm);
    if (s_factor > 0.0f) {
        s_smooth_dx = s_smooth_dx * s_factor + dx * (1.0f - s_factor);
        s_smooth_dy = s_smooth_dy * s_factor + dy * (1.0f - s_factor);
        if (fabsf(s_smooth_dx) < 0.001f) s_smooth_dx = 0.0f;
        if (fabsf(s_smooth_dy) < 0.001f) s_smooth_dy = 0.0f;
        dx = s_smooth_dx;
        dy = s_smooth_dy;
    } else {
        s_smooth_dx = 0.0f;
        s_smooth_dy = 0.0f;
    }

    if (dx == 0.0f && dy == 0.0f) return 0;

    /* Scaled delta for camera angles and stick vector injection */
    const float base_sens = 0.0015f * responsiveness;
    delta[1] = (inv_x ? -1.0f : 1.0f) * dx * sens_x * base_sens;
    delta[0] = (inv_y ? -1.0f : 1.0f) * dy * sens_y * base_sens;
    return 1;
}

static void follow_cam_apply_delta(uintptr_t cam_addr, float *stick, float dt, const float delta[2]) {
    /* The engine rebuilds pitch/yaw from the camera position each update, so only the stick moves it.
     * Above |stick| 0.998 it turns at a fixed rate that ramps up while held and resets on reversal;
     * below, it turns by stick * speed * dt. Use a half-tilt stick and set speed so that equals the delta;
     * the original speeds are restored at the next update. */
    if (!stick) return;
    if (!(dt > 0.0f && dt < 0.5f)) dt = 1.0f / 30.0f;

    for (int i = 0; i < 4; ++i) g_saved_speed[i] = *(float*)(cam_addr + k_cam_speed_offs[i]);
    g_boosted_cam = cam_addr;
    for (int axis = 0; axis < 2; ++axis) {
        g_stick_added[axis] = 0.0f;
        if (delta[axis] == 0.0f) continue;
        const float speed = 2.0f * fabsf(delta[axis]) / dt;
        *(float*)(cam_addr + k_cam_speed_offs[2 * axis]) = speed;
        *(float*)(cam_addr + k_cam_speed_offs[2 * axis + 1]) = speed;
        g_stick_added[axis] = delta[axis] > 0.0f ? 0.5f : -0.5f;
        stick[axis] += g_stick_added[axis];
    }
    g_stick_buf = stick;
    g_stick_left[0] = stick[0];
    g_stick_left[1] = stick[1];
}

/* The aim camera keeps its own {pitch, yaw} at +0x148 and adds stick * speed (+0xfc, +0x100) * dt
 * each update, then wraps and clamps them; it reads the stick only while +0x188 is set. */
void host_aim_mouse_inject(void *cam, void *rsi_ctx) {
    const uintptr_t cam_addr = (uintptr_t)cam;
    if (cam_addr < 0x10000) return;
    remove_stick_offset(camera_stick_buf(rsi_ctx));
    if (!native_camera_is_active() || !bbgpu_mouse_captured()) return;

    float delta[2];
    if (!mouse_look_delta(delta)) return;
    if (*(volatile uint8_t*)(cam_addr + 0x188) == 0) return;

    /* The game slows the stick by current/max field of view while zoomed; match it. */
    float zoom = *(float*)(cam_addr + 0x50) / *(float*)(cam_addr + 0x134);
    if (!(zoom > 0.0f && zoom <= 4.0f)) zoom = 1.0f;

    float *angles = (float*)(cam_addr + 0x148);
    const float *speed = (const float*)(cam_addr + 0xfc);
    for (int axis = 0; axis < 2; ++axis)
        angles[axis] += (speed[axis] < 0.0f ? -delta[axis] : delta[axis]) * zoom;
}

__attribute__((naked)) void guest_camera_update_thunk(void) {
    __asm__ __volatile__(
        /* Save caller-save registers and camera pointer (SysV ABI: rdi=cam, rsi=ctx) */
        "pushfq\n\t"
        "push %%rax\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "sub $296, %%rsp\n\t"
        "movdqu %%xmm0,   32(%%rsp)\n\t"
        "movdqu %%xmm1,   48(%%rsp)\n\t"
        "movdqu %%xmm2,   64(%%rsp)\n\t"
        "movdqu %%xmm3,   80(%%rsp)\n\t"
        "movdqu %%xmm4,   96(%%rsp)\n\t"
        "movdqu %%xmm5,  112(%%rsp)\n\t"
        "movdqu %%xmm6,  128(%%rsp)\n\t"
        "movdqu %%xmm7,  144(%%rsp)\n\t"
        "movdqu %%xmm8,  160(%%rsp)\n\t"
        "movdqu %%xmm9,  176(%%rsp)\n\t"
        "movdqu %%xmm10, 192(%%rsp)\n\t"
        "movdqu %%xmm11, 208(%%rsp)\n\t"
        "movdqu %%xmm12, 224(%%rsp)\n\t"
        "movdqu %%xmm13, 240(%%rsp)\n\t"
        "movdqu %%xmm14, 256(%%rsp)\n\t"
        "movdqu %%xmm15, 272(%%rsp)\n\t"

        /* 1. Inject mouse deltas into camera stick buffer and angles before update runs */
        "mov 328(%%rsp), %%rcx\n\t" /* saved rdi (cam) -> Windows arg0 (rcx) */
        "mov 336(%%rsp), %%rdx\n\t" /* saved rsi (ctx) -> Windows arg1 (rdx) */
        "movss 32(%%rsp), %%xmm2\n\t" /* guest xmm0 (frame dt) -> Windows arg2 (xmm2) */
        "call host_native_mouse_inject\n\t"

        /* 2. Restore all registers and pass through to original camera update */
        "movdqu  32(%%rsp), %%xmm0\n\t"
        "movdqu  48(%%rsp), %%xmm1\n\t"
        "movdqu  64(%%rsp), %%xmm2\n\t"
        "movdqu  80(%%rsp), %%xmm3\n\t"
        "movdqu  96(%%rsp), %%xmm4\n\t"
        "movdqu 112(%%rsp), %%xmm5\n\t"
        "movdqu 128(%%rsp), %%xmm6\n\t"
        "movdqu 144(%%rsp), %%xmm7\n\t"
        "movdqu 160(%%rsp), %%xmm8\n\t"
        "movdqu 176(%%rsp), %%xmm9\n\t"
        "movdqu 192(%%rsp), %%xmm10\n\t"
        "movdqu 208(%%rsp), %%xmm11\n\t"
        "movdqu 224(%%rsp), %%xmm12\n\t"
        "movdqu 240(%%rsp), %%xmm13\n\t"
        "movdqu 256(%%rsp), %%xmm14\n\t"
        "movdqu 272(%%rsp), %%xmm15\n\t"
        "add $296, %%rsp\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rax\n\t"
        "popfq\n\t"
        "jmp *orig_cam_update(%%rip)\n\t"
        : : : "memory"
    );
}

/* SysV guest -> Win64 host: keeps the guest argument registers (rdi, rsi, rdx, rcx, r8, r9, xmm0-7)
 * that Win64 lets the host clobber or that the aim update reads (rdi=cam, rsi=ctx, xmm0=dt). */
__attribute__((naked)) void guest_aim_camera_update_thunk(void) {
    __asm__ __volatile__(
        "push %%rdi\n\t"
        "push %%rsi\n\t"
        "push %%rdx\n\t"
        "push %%rcx\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "sub $168, %%rsp\n\t" /* 32 shadow + 8 x 16 xmm + 8 to align */
        "movdqu %%xmm0,  32(%%rsp)\n\t"
        "movdqu %%xmm1,  48(%%rsp)\n\t"
        "movdqu %%xmm2,  64(%%rsp)\n\t"
        "movdqu %%xmm3,  80(%%rsp)\n\t"
        "movdqu %%xmm4,  96(%%rsp)\n\t"
        "movdqu %%xmm5, 112(%%rsp)\n\t"
        "movdqu %%xmm6, 128(%%rsp)\n\t"
        "movdqu %%xmm7, 144(%%rsp)\n\t"
        "mov %%rdi, %%rcx\n\t"
        "mov %%rsi, %%rdx\n\t"
        "call host_aim_mouse_inject\n\t"
        "movdqu  32(%%rsp), %%xmm0\n\t"
        "movdqu  48(%%rsp), %%xmm1\n\t"
        "movdqu  64(%%rsp), %%xmm2\n\t"
        "movdqu  80(%%rsp), %%xmm3\n\t"
        "movdqu  96(%%rsp), %%xmm4\n\t"
        "movdqu 112(%%rsp), %%xmm5\n\t"
        "movdqu 128(%%rsp), %%xmm6\n\t"
        "movdqu 144(%%rsp), %%xmm7\n\t"
        "add $168, %%rsp\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rcx\n\t"
        "pop %%rdx\n\t"
        "pop %%rsi\n\t"
        "pop %%rdi\n\t"
        "jmp *orig_aim_cam_update(%%rip)\n\t"
        : : : "memory"
    );
}

static void print_exit_summary(void) {
    static int summary_printed = 0;
    if (summary_printed) return;
    summary_printed = 1;
    uint64_t total_s = g_stat_total_seconds;
    if (total_s == 0) return;
    uint64_t total_flips = gpu_enabled ? bbgpu_get_flip_count() : 0;
    double avg_flips_per_s = (double)total_flips / (double)total_s;
    double zero_flip_pct = (double)g_stat_zero_flip_seconds * 100.0 / (double)total_s;
    uint64_t max_freeze = g_stat_longest_freeze_s;
    if (g_stat_current_freeze_s > max_freeze) max_freeze = g_stat_current_freeze_s;

    printf("\n================================================================================\n");
    printf("=== BLOODBORNE RUN SUMMARY ===\n");
    printf("  Total Runtime:       %llu s\n", (unsigned long long)total_s);
    printf("  Total Flips:         %llu\n", (unsigned long long)total_flips);
    printf("  Average Flips/s:     %.2f\n", avg_flips_per_s);
    printf("  Zero-Flip Seconds:   %llu of %llu (%.1f%%)\n",
           (unsigned long long)g_stat_zero_flip_seconds, (unsigned long long)total_s, zero_flip_pct);
    printf("  Longest Freeze:      %llu s\n", (unsigned long long)max_freeze);
    printf("================================================================================\n\n");
    fflush(stdout);

    FILE *f = fopen("out/summary.log", "w");
    if (f) {
        fprintf(f, "=== BLOODBORNE RUN SUMMARY ===\n");
        fprintf(f, "Total Runtime:       %llu s\n", (unsigned long long)total_s);
        fprintf(f, "Total Flips:         %llu\n", (unsigned long long)total_flips);
        fprintf(f, "Average Flips/s:     %.2f\n", avg_flips_per_s);
        fprintf(f, "Zero-Flip Seconds:   %llu of %llu (%.1f%%)\n",
                (unsigned long long)g_stat_zero_flip_seconds, (unsigned long long)total_s, zero_flip_pct);
        fprintf(f, "Longest Freeze:      %llu s\n", (unsigned long long)max_freeze);
        fclose(f);
    }
}
int vulkan_smoke(void);


static void fail(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 1);
#else
    exit(1);
#endif
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) {
        CHECK_LOW_ADDR(low);
        return low;
    }
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        DWORD err = GetLastError();
        fprintf(stderr, "Runtime ERROR: VirtualAlloc(size %zu) failed: Win32 error %lu\n", size, (unsigned long)err);
        fail("VirtualAlloc failed");
    }
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    CHECK_LOW_ADDR(p);
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static volatile uint64_t g_last_progress_tick = 0;
static volatile int g_watchdog_running = 1;

void runtime_notify_progress(void) {
#ifdef _WIN32
    g_last_progress_tick = GetTickCount64();
#endif
}

ABI void restore_guest_fs(void) {
#ifdef _WIN32
    void *tcb = runtime_thread_get_tcb();
    if (tcb && runtime_win_has_fsgsbase()) {
        __asm__ __volatile__("wrfsbase %0" : : "r"(tcb));
    }
#endif
}

static inline int check_noisy_import(const char *sym) {
    if (!sym) return 0;
    return (!strcmp(sym, "memcpy") || !strcmp(sym, "memset") || !strcmp(sym, "memcmp") ||
            !strcmp(sym, "memmove") || !strcmp(sym, "memchr") || !strcmp(sym, "strlen") ||
            !strcmp(sym, "strcmp") || !strcmp(sym, "strncmp") || !strcmp(sym, "strcpy") ||
            !strcmp(sym, "wmemcmp") || !strcmp(sym, "wmemcpy") || !strcmp(sym, "wmemset") ||
            !strcmp(sym, "wcslen") || !strcmp(sym, "wmemchr") || !strcmp(sym, "wcschr") ||
            !strcmp(sym, "pthread_mutex_lock") || !strcmp(sym, "pthread_mutex_unlock") ||
            !strcmp(sym, "scePthreadMutexLock") || !strcmp(sym, "scePthreadMutexUnlock") ||
            !strcmp(sym, "sceKernelReadTsc") || !strcmp(sym, "sceKernelGetProcessTimeCounter") ||
            !strcmp(sym, "scePthreadSelf") || !strcmp(sym, "_Getpctype"));
}

ABI void *pre_import_hook(uint32_t index, uint64_t *args, void *caller) {
    runtime_notify_progress();
    if (index >= import_count) return NULL;
    uint64_t total = ++g_total_import_calls;
    uint64_t count = ++import_call_counts[index];
    const char *name = names[index];
    const char *sym = runtime_import_name(name);
    uintptr_t caller_off = (uintptr_t)caller >= (uintptr_t)image && (uintptr_t)caller < (uintptr_t)image + 0x20000000
                           ? (uintptr_t)caller - (uintptr_t)image : (uintptr_t)caller;
    int is_noisy = check_noisy_import(sym);

    GuestThread *cur = runtime_thread_current();
    if (cur) {
        uint32_t ring_slot = cur->recent_import_count++;
        cur->recent_imports[ring_slot & 31] = index;
    }
    if (cur && cur->name[0] == 'M' && !strncmp(cur->name, "MOMainThread", 12)) {
        uint32_t slot = (mo_recent_count++) % MO_RING_CAP;
        mo_recent_calls[slot].index = index;
        for (int a = 0; a < 6; ++a) mo_recent_calls[slot].args[a] = args[a];
        mo_total_import_count++;
        if (mo_call_counts && index < import_count) {
            mo_call_counts[index]++;
        }
    }

    if (g_import_file && g_import_file_lines < 100000) {
        if (!is_noisy || count <= 5) {
            fprintf(g_import_file, "[#%" PRIu64 "] [idx %u] %s (%s) caller=0x%" PRIxPTR " args=(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
                    total, index, name, sym ? sym : "unknown", caller_off,
                    args[0], args[1], args[2], args[3], args[4], args[5]);
            g_import_file_lines++;
            if (g_import_file_lines == 100000) {
                fprintf(g_import_file, "[IMPORT LOG CAPPED AT 100,000 ENTRIES TO PREVENT OOM]\n");
            }
        }
    }

    if (is_unbound[index]) {
        if (g_bb_trace && count <= 3) {
            printf("[UNBOUND IMPORT CALLED #%" PRIu64 "] index %u: %s (%s) from 0x%" PRIxPTR "\n"
                   "  args: (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ") -> returning 0\n",
                   count, index, name, sym ? sym : "unknown", caller_off,
                   args[0], args[1], args[2], args[3], args[4], args[5]);
            fflush(stdout);
        }
        return NULL;
    }

    if (g_bb_trace && ((is_noisy && count == 1) || (!is_noisy && count <= 5))) {
        printf("[IMPORT CALL #%" PRIu64 "] %s (%s) from 0x%" PRIxPTR "\n"
               "  args: (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
               count, name, sym ? sym : "unknown", caller_off,
               args[0], args[1], args[2], args[3], args[4], args[5]);
        fflush(stdout);
    }
    return resolved_targets[index];
}

ABI void post_import_hook(uint32_t index, uint64_t ret_val) {
    if (index >= import_count) return;
    uint64_t count = import_call_counts[index];
    const char *name = names[index];
    const char *sym = runtime_import_name(name);
    int is_noisy = check_noisy_import(sym);

    if (g_import_file && g_import_file_lines < 100000) {
        if (!is_noisy || count <= 5) {
            fprintf(g_import_file, "  -> [#%" PRIu64 " RET] %s (%s) ret=0x%" PRIx64 "\n",
                    count, name, sym ? sym : "unknown", ret_val);
            g_import_file_lines++;
        }
    }

    if ((is_noisy && count == 1) || (!is_noisy && count <= 5)) {
        printf("[IMPORT RET #%" PRIu64 "] %s (%s) -> 0x%" PRIx64 "\n",
               count, name, sym ? sym : "unknown", ret_val);
        fflush(stdout);
    }
}

void common_dispatch(void);
__asm__(
".text\n"
".globl common_dispatch\n"
"common_dispatch:\n"
"   push %rbp\n"
"   mov %rsp, %rbp\n"
"   sub $208, %rsp\n"
"   mov %r11, 0(%rsp)\n"
"   mov %rdi, 8(%rsp)\n"
"   mov %rsi, 16(%rsp)\n"
"   mov %rdx, 24(%rsp)\n"
"   mov %rcx, 32(%rsp)\n"
"   mov %r8,  40(%rsp)\n"
"   mov %r9,  48(%rsp)\n"
"   mov %rax, 56(%rsp)\n"
"   mov %r10, 64(%rsp)\n"
"   movdqu %xmm0, 80(%rsp)\n"
"   movdqu %xmm1, 96(%rsp)\n"
"   movdqu %xmm2, 112(%rsp)\n"
"   movdqu %xmm3, 128(%rsp)\n"
"   movdqu %xmm4, 144(%rsp)\n"
"   movdqu %xmm5, 160(%rsp)\n"
"   movdqu %xmm6, 176(%rsp)\n"
"   movdqu %xmm7, 192(%rsp)\n"
"   mov 0(%rsp), %rdi\n"
"   lea 8(%rsp), %rsi\n"
"   mov 8(%rbp), %rdx\n"
"   call pre_import_hook\n"
"   test %rax, %rax\n"
"   jnz 1f\n"
"   xor %rax, %rax\n"
"   xor %rdx, %rdx\n"
"   pxor %xmm0, %xmm0\n"
"   add $208, %rsp\n"
"   pop %rbp\n"
"   ret\n"
"1:\n"
"   mov %rax, %r11\n"
"   sub $64, %rsp\n"
"   mov 16(%rbp), %rax\n"
"   mov %rax, 0(%rsp)\n"
"   mov 24(%rbp), %rax\n"
"   mov %rax, 8(%rsp)\n"
"   mov 32(%rbp), %rax\n"
"   mov %rax, 16(%rsp)\n"
"   mov 40(%rbp), %rax\n"
"   mov %rax, 24(%rsp)\n"
"   mov 48(%rbp), %rax\n"
"   mov %rax, 32(%rsp)\n"
"   mov 56(%rbp), %rax\n"
"   mov %rax, 40(%rsp)\n"
"   mov 64(%rbp), %rax\n"
"   mov %rax, 48(%rsp)\n"
"   mov 72(%rbp), %rax\n"
"   mov %rax, 56(%rsp)\n"
"   movdqu 80+64(%rsp), %xmm0\n"
"   movdqu 96+64(%rsp), %xmm1\n"
"   movdqu 112+64(%rsp), %xmm2\n"
"   movdqu 128+64(%rsp), %xmm3\n"
"   movdqu 144+64(%rsp), %xmm4\n"
"   movdqu 160+64(%rsp), %xmm5\n"
"   movdqu 176+64(%rsp), %xmm6\n"
"   movdqu 192+64(%rsp), %xmm7\n"
"   mov 8+64(%rsp),  %rdi\n"
"   mov 16+64(%rsp), %rsi\n"
"   mov 24+64(%rsp), %rdx\n"
"   mov 32+64(%rsp), %rcx\n"
"   mov 40+64(%rsp), %r8\n"
"   mov 48+64(%rsp), %r9\n"
"   mov 56+64(%rsp), %rax\n"
"   mov 64+64(%rsp), %r10\n"
"   call *%r11\n"
"   add $64, %rsp\n"
"   mov %rax, 56(%rsp)\n"
"   mov %rdx, 24(%rsp)\n"
"   movdqu %xmm0, 80(%rsp)\n"
"   mov 0(%rsp), %rdi\n"
"   mov %rax, %rsi\n"
"   call post_import_hook\n"
"   call restore_guest_fs\n"
"   mov 56(%rsp), %rax\n"
"   mov 24(%rsp), %rdx\n"
"   movdqu 80(%rsp), %xmm0\n"
"   add $208, %rsp\n"
"   pop %rbp\n"
"   ret\n"
);

static void print_code_loc(const char *prefix, uintptr_t addr) {
    if (addr >= (uintptr_t)image && addr < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
        uintptr_t off = addr - (uintptr_t)image;
        for (uint64_t m = 0; m < module_count; ++m) {
            if (off >= modules[m].base && off < modules[m].base + modules[m].size) {
                printf("%smodule %" PRIu64 " (%s) +0x%" PRIxPTR "\n", prefix, m, m == 0 ? "libc.prx" : "system module", off - modules[m].base);
                return;
            }
        }
        printf("%sguest eboot.bin +0x%" PRIxPTR "\n", prefix, off);
    } else {
        printf("%shost address 0x%" PRIxPTR "\n", prefix, addr);
    }
}

static void dump_watchdog(void) {
    printf("\n================================================================================\n");
    printf(">>> WATCHDOG: NO PROGRESS FOR 5 SECONDS! THREAD & IMPORT DUMP <<<\n");
    printf("================================================================================\n");

    pthread_mutex_t *tlock = (pthread_mutex_t *)runtime_thread_get_lock();
    if (tlock) pthread_mutex_lock(tlock);

    for (GuestThread *t = runtime_thread_get_all(); t; t = t->next) {
        if (t->finished) continue;
        printf("\n--- Thread '%s' (TID %lu, host_owned=%d) ---\n", t->name, (unsigned long)t->win32_tid, t->host_owned);
        if (t->blocked_on) {
            printf("  State: BLOCKED on %s (resource 0x%" PRIx64 ")\n", t->blocked_on, t->blocked_resource);
        } else {
            printf("  State: RUNNING / SPINNING\n");
        }

#ifdef _WIN32
        if (t->win32_handle) {
            SuspendThread(t->win32_handle);
            CONTEXT ctx = {0};
            ctx.ContextFlags = CONTEXT_ALL;
            if (GetThreadContext(t->win32_handle, &ctx)) {
                printf("  Registers:\n");
                printf("    RIP=0x%016llx  RSP=0x%016llx  RBP=0x%016llx\n",
                       (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp);
                printf("    RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx  RDX=0x%016llx\n",
                       (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx, (unsigned long long)ctx.Rcx, (unsigned long long)ctx.Rdx);
                printf("    RSI=0x%016llx  RDI=0x%016llx  R8 =0x%016llx  R9 =0x%016llx\n",
                       (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi, (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);
                printf("    R10=0x%016llx  R11=0x%016llx  R12=0x%016llx  R13=0x%016llx\n",
                       (unsigned long long)ctx.R10, (unsigned long long)ctx.R11, (unsigned long long)ctx.R12, (unsigned long long)ctx.R13);
                printf("    R14=0x%016llx  R15=0x%016llx  EFLAGS=0x%08lx\n",
                       (unsigned long long)ctx.R14, (unsigned long long)ctx.R15, (unsigned long)ctx.EFlags);

                print_code_loc("  RIP Location: ", (uintptr_t)ctx.Rip);

                unsigned char rip_bytes[16] = {0};
                if (ReadProcessMemory(GetCurrentProcess(), (void *)ctx.Rip, rip_bytes, 16, NULL)) {
                    printf("  Bytes at RIP: ");
                    for (int b = 0; b < 16; ++b) printf("%02x ", rip_bytes[b]);
                    printf("\n");
                }

                printf("  Stack Walk (RBP chain):\n");
                uintptr_t cur_rbp = ctx.Rbp;
                for (int f = 0; f < 20 && cur_rbp; ++f) {
                    uintptr_t next_rbp = 0, ret_addr = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)cur_rbp, &next_rbp, 8, NULL) ||
                        !ReadProcessMemory(GetCurrentProcess(), (void *)(cur_rbp + 8), &ret_addr, 8, NULL)) {
                        break;
                    }
                    char pfx[32];
                    snprintf(pfx, sizeof(pfx), "    #%d 0x%016llx: ", f, (unsigned long long)ret_addr);
                    print_code_loc(pfx, ret_addr);
                    if (next_rbp <= cur_rbp || next_rbp > cur_rbp + 0x1000000) break;
                    cur_rbp = next_rbp;
                }

                printf("  Stack Scan (code pointers on stack):\n");
                uintptr_t stack_vals[256];
                SIZE_T read_b = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (void *)ctx.Rsp, stack_vals, sizeof(stack_vals), &read_b)) {
                    int found = 0;
                    for (size_t s = 0; s < read_b / 8 && found < 20; ++s) {
                        uintptr_t val = stack_vals[s];
                        if (val >= (uintptr_t)image && val < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
                            char pfx[64];
                            snprintf(pfx, sizeof(pfx), "    [RSP+0x%x] = 0x%016llx: ", (unsigned)(s * 8), (unsigned long long)val);
                            print_code_loc(pfx, val);
                            found++;
                        }
                    }
                }
            }
            ResumeThread(t->win32_handle);
        }
#endif
    }
    if (tlock) pthread_mutex_unlock(tlock);

    printf("\n--- UNBOUND IMPORTS CALLED (Sorted by Call Count) ---\n");
    int any_unbound = 0;
    typedef struct { uint32_t idx; uint64_t count; } ImportStat;
    ImportStat stats[1024];
    uint32_t stat_count = 0;
    for (uint64_t i = 0; i < import_count && stat_count < 1024; ++i) {
        if (is_unbound[i] && import_call_counts[i] > 0) {
            stats[stat_count++] = (ImportStat){(uint32_t)i, import_call_counts[i]};
            any_unbound = 1;
        }
    }
    for (uint32_t a = 0; a < stat_count; ++a) {
        for (uint32_t b = a + 1; b < stat_count; ++b) {
            if (stats[b].count > stats[a].count) {
                ImportStat tmp = stats[a]; stats[a] = stats[b]; stats[b] = tmp;
            }
        }
    }
    for (uint32_t a = 0; a < stat_count; ++a) {
        uint32_t i = stats[a].idx;
        printf("  %5llu calls: %s (%s)\n", (unsigned long long)stats[a].count, names[i], runtime_import_name(names[i]));
    }
    if (!any_unbound) printf("  (None called)\n");

    printf("\n--- TOP HOST IMPORTS CALLED ---\n");
    stat_count = 0;
    for (uint64_t i = 0; i < import_count && stat_count < 1024; ++i) {
        if (!is_unbound[i] && import_call_counts[i] > 0) {
            stats[stat_count++] = (ImportStat){(uint32_t)i, import_call_counts[i]};
        }
    }
    for (uint32_t a = 0; a < stat_count; ++a) {
        for (uint32_t b = a + 1; b < stat_count; ++b) {
            if (stats[b].count > stats[a].count) {
                ImportStat tmp = stats[a]; stats[a] = stats[b]; stats[b] = tmp;
            }
        }
    }
    uint32_t limit = stat_count < 20 ? stat_count : 20;
    for (uint32_t a = 0; a < limit; ++a) {
        uint32_t i = stats[a].idx;
        printf("  %8llu calls: %s (%s)\n", (unsigned long long)stats[a].count, names[i], runtime_import_name(names[i]));
    }

    printf("================================================================================\n\n");
    fflush(stdout);
}

static char g_run_timestamp[32];

static void dump_hang_snapshot(const char *path, uint64_t elapsed_s, uint64_t cur_flips, uint64_t cur_submits, uint64_t now) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "================================================================================\n");
    fprintf(f, "=== BLOODBORNE HANG SNAPSHOT (Elapsed %llus, Flips: %llu, Submits: %llu) ===\n",
            (unsigned long long)elapsed_s, (unsigned long long)cur_flips, (unsigned long long)cur_submits);
    fprintf(f, "================================================================================\n\n");

    pthread_mutex_t *tlock = (pthread_mutex_t *)runtime_thread_get_lock();
    if (tlock) pthread_mutex_lock(tlock);

    for (GuestThread *t = runtime_thread_get_all(); t; t = t->next) {
        if (t->finished) continue;
        uint64_t wait_ms = (t->blocked_tick && now >= t->blocked_tick) ? (now - t->blocked_tick) : 0;
        const char *wait_reason = t->blocked_on ? t->blocked_on : "running/unblocked";
        uint64_t wait_res = t->blocked_resource;

        uintptr_t rip = 0, rsp = 0, rbp = 0;
#ifdef _WIN32
        HANDLE th = t->win32_handle;
        DWORD tid = t->win32_tid;
        int need_close = 0;
        if (!th && tid) {
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
            need_close = 1;
        }
        if (th && tid != GetCurrentThreadId()) {
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx;
                memset(&ctx, 0, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_ALL;
                if (GetThreadContext(th, &ctx)) {
                    rip = ctx.Rip;
                    rsp = ctx.Rsp;
                    rbp = ctx.Rbp;
                }
                ResumeThread(th);
            }
        }
        if (need_close && th) CloseHandle(th);
#endif
        fprintf(f, "Thread '%s' (TID %lu, host_owned=%d):\n", t->name, (unsigned long)t->win32_tid, t->host_owned);
        fprintf(f, "  Wait State: %s (resource 0x%" PRIx64 "), waited: %.2fs\n",
                wait_reason, wait_res, (double)wait_ms / 1000.0);
        if (t->blocked_on) {
            if (!strcmp(t->blocked_on, "rwlock_read") || !strcmp(t->blocked_on, "rwlock_write")) {
                runtime_rwlock_dump_info(f, (void *)(uintptr_t)wait_res);
            } else if (!strcmp(t->blocked_on, "mutex") || !strcmp(t->blocked_on, "mutex_timed")) {
                runtime_mutex_dump_info(f, (void *)(uintptr_t)wait_res);
            }
        }

        char loc[256];
        if (rip >= (uintptr_t)image && rip < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
            uintptr_t off = rip - (uintptr_t)image;
            int found_mod = 0;
            for (uint64_t m = 0; m < module_count; ++m) {
                if (off >= modules[m].base && off < modules[m].base + modules[m].size) {
                    snprintf(loc, sizeof(loc), "module %" PRIu64 " (%s) +0x%" PRIxPTR,
                             m, m == 0 ? "libc.prx" : "system module", off - modules[m].base);
                    found_mod = 1;
                    break;
                }
            }
            if (!found_mod) snprintf(loc, sizeof(loc), "guest eboot.bin +0x%" PRIxPTR, off);
        } else if (rip) {
            snprintf(loc, sizeof(loc), "host address 0x%" PRIxPTR, rip);
        } else {
            snprintf(loc, sizeof(loc), "unknown / current thread");
        }
        fprintf(f, "  Top frame: RIP=0x%" PRIxPTR " (%s), RSP=0x%" PRIxPTR ", RBP=0x%" PRIxPTR "\n", rip, loc, rsp, rbp);

        if (rsp && rsp < (1ULL << 40)) {
            uintptr_t *stack_ptr = (uintptr_t *)rsp;
            if (!IsBadReadPtr(stack_ptr, 32 * sizeof(uintptr_t))) {
                int frames_printed = 0;
                for (int si = 0; si < 32 && frames_printed < 8; ++si) {
                    uintptr_t val = stack_ptr[si];
                    if (val >= (uintptr_t)image && val < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
                        uintptr_t off = val - (uintptr_t)image;
                        fprintf(f, "    [frame +0x%x] 0x%" PRIxPTR " (guest eboot.bin +0x%" PRIxPTR ")\n", si * 8, val, off);
                        frames_printed++;
                    }
                }
            }
        }
        fprintf(f, "\n");
    }
    if (tlock) pthread_mutex_unlock(tlock);

    if (gpu_enabled) {
        bbgpu_dump_host_threads_hang(f);
        bbgpu_dump_breadcrumbs(f);
    }

    fprintf(f, "=== MOMAINTHREAD RECENT IMPORTS (Total Calls: %llu) ===\n", (unsigned long long)mo_total_import_count);
    uint32_t n_calls = mo_recent_count < MO_RING_CAP ? mo_recent_count : MO_RING_CAP;
    uint32_t start_idx = mo_recent_count > MO_RING_CAP ? (mo_recent_count - MO_RING_CAP) : 0;
    for (uint32_t c = 0; c < n_calls; ++c) {
        uint32_t idx_in_ring = (start_idx + c) % MO_RING_CAP;
        uint32_t imp_idx = mo_recent_calls[idx_in_ring].index;
        const char *name = imp_idx < import_count ? names[imp_idx] : "unknown";
        const char *sym = runtime_import_name(name);
        uint64_t *a = mo_recent_calls[idx_in_ring].args;
        fprintf(f, "  #%u: %s (%s) args=(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
                c + 1, name, sym ? sym : "unknown", a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (mo_call_counts) {
        fprintf(f, "\n=== MOMAINTHREAD TOP REPEATED IMPORTS ===\n");
        typedef struct { uint32_t idx; uint64_t count; } MoTop;
        MoTop top[64];
        uint32_t top_len = 0;
        for (uint64_t i = 0; i < import_count; ++i) {
            if (mo_call_counts[i] > 0) {
                if (top_len < 64) {
                    top[top_len++] = (MoTop){(uint32_t)i, mo_call_counts[i]};
                } else {
                    uint32_t min_j = 0;
                    for (uint32_t j = 1; j < 64; ++j) {
                        if (top[j].count < top[min_j].count) min_j = j;
                    }
                    if (mo_call_counts[i] > top[min_j].count) {
                        top[min_j] = (MoTop){(uint32_t)i, mo_call_counts[i]};
                    }
                }
            }
        }
        for (uint32_t a = 0; a < top_len; ++a) {
            for (uint32_t b = a + 1; b < top_len; ++b) {
                if (top[b].count > top[a].count) {
                    MoTop tmp = top[a]; top[a] = top[b]; top[b] = tmp;
                }
            }
        }
        for (uint32_t a = 0; a < (top_len < 10 ? top_len : 10); ++a) {
            uint32_t i = top[a].idx;
            fprintf(f, "  %llu calls: %s (%s)\n", (unsigned long long)top[a].count, names[i], runtime_import_name(names[i]));
        }
    }
    fprintf(f, "\n");

    fprintf(f, "=== END SNAPSHOT ===\n");
    fflush(f);
    fclose(f);
    printf("\n[WATCHDOG] Flips stalled for 5s: snapshot written to %s\n\n", path);
    fflush(stdout);
}

#ifdef _WIN32
static DWORD WINAPI watchdog_worker(LPVOID param) {
    (void)param;
    uint64_t last_flips = 0;
    uint64_t last_submits = 0;
    uint64_t last_imports = 0;
    uint64_t start_tick = GetTickCount64();
    uint64_t last_dump_tick = 0;
    uint64_t zero_flip_seconds = 0;
    int hang_dump_written = 0;

    char hb_ts_path[128];
    if (g_run_timestamp[0]) snprintf(hb_ts_path, sizeof(hb_ts_path), "out/heartbeat_%s.log", g_run_timestamp);
    else snprintf(hb_ts_path, sizeof(hb_ts_path), "out/heartbeat.log");

    FILE *f_heartbeat_ts = (g_bb_trace && g_run_timestamp[0]) ? fopen(hb_ts_path, "w") : NULL;
    FILE *f_heartbeat = g_bb_trace ? fopen("out/heartbeat.log", "w") : NULL;

    while (g_watchdog_running) {
        Sleep(1000);
        uint64_t now = GetTickCount64();
        uint64_t elapsed_s = (now - start_tick) / 1000;

        uint64_t cur_flips = gpu_enabled ? bbgpu_get_flip_count() : 0;
        uint64_t cur_submits = gpu_enabled ? bbgpu_get_submit_count() : 0;
        uint64_t cur_imports = g_total_import_calls;

        uint64_t flip_delta = cur_flips >= last_flips ? cur_flips - last_flips : 0;
        uint64_t submit_delta = cur_submits >= last_submits ? cur_submits - last_submits : 0;
        uint64_t import_delta = cur_imports >= last_imports ? cur_imports - last_imports : 0;

        last_flips = cur_flips;
        last_submits = cur_submits;
        last_imports = cur_imports;

        g_stat_total_seconds = elapsed_s;
        if (cur_flips == 0 || flip_delta == 0) {
            zero_flip_seconds++;
            g_stat_zero_flip_seconds++;
            g_stat_current_freeze_s = zero_flip_seconds;
            if (zero_flip_seconds > g_stat_longest_freeze_s) {
                g_stat_longest_freeze_s = zero_flip_seconds;
            }
        } else {
            zero_flip_seconds = 0;
            g_stat_current_freeze_s = 0;
            hang_dump_written = 0;
        }

        if (g_bb_trace) {
            if (zero_flip_seconds >= 5 && !hang_dump_written) {
                hang_dump_written = 1;
                char dump_ts_path[128];
                if (g_run_timestamp[0]) {
                    snprintf(dump_ts_path, sizeof(dump_ts_path), "out/hang_dump_%s.log", g_run_timestamp);
                    dump_hang_snapshot(dump_ts_path, elapsed_s, cur_flips, cur_submits, now);
                }
                dump_hang_snapshot("out/hang_dump.log", elapsed_s, cur_flips, cur_submits, now);
            }

            pthread_mutex_t *tlock = (pthread_mutex_t *)runtime_thread_get_lock();
            if (tlock) pthread_mutex_lock(tlock);

            uint32_t active_threads = 0;
            for (GuestThread *t = runtime_thread_get_all(); t; t = t->next) {
                if (!t->finished) active_threads++;
            }

            printf("[HEARTBEAT %llus] flips: %llu (+%llu/s) | gpu_submits: %llu (+%llu/s) | imports/s: %llu | threads: %u\n",
                   (unsigned long long)elapsed_s,
                   (unsigned long long)cur_flips, (unsigned long long)flip_delta,
                   (unsigned long long)cur_submits, (unsigned long long)submit_delta,
                   (unsigned long long)import_delta,
                   active_threads);

            if (f_heartbeat) {
                fprintf(f_heartbeat, "[HEARTBEAT %llus] flips: %llu (+%llu/s) | gpu_submits: %llu (+%llu/s) | imports/s: %llu | threads: %u\n",
                        (unsigned long long)elapsed_s,
                        (unsigned long long)cur_flips, (unsigned long long)flip_delta,
                        (unsigned long long)cur_submits, (unsigned long long)submit_delta,
                        (unsigned long long)import_delta,
                        active_threads);
            }
            if (f_heartbeat_ts) {
                fprintf(f_heartbeat_ts, "[HEARTBEAT %llus] flips: %llu (+%llu/s) | gpu_submits: %llu (+%llu/s) | imports/s: %llu | threads: %u\n",
                        (unsigned long long)elapsed_s,
                        (unsigned long long)cur_flips, (unsigned long long)flip_delta,
                        (unsigned long long)cur_submits, (unsigned long long)submit_delta,
                        (unsigned long long)import_delta,
                        active_threads);
            }

            for (GuestThread *t = runtime_thread_get_all(); t; t = t->next) {
                if (!t->finished && t->blocked_on && t->blocked_tick && (now - t->blocked_tick >= 3000)) {
                    double wait_s = (double)(now - t->blocked_tick) / 1000.0;
                    printf("  [BLOCKED >3s] Thread '%s' (TID %lu) waiting on %s (res 0x%" PRIx64 ") for %.1fs\n",
                           t->name, (unsigned long)t->win32_tid, t->blocked_on, t->blocked_resource, wait_s);
                    if (f_heartbeat) {
                        fprintf(f_heartbeat, "  [BLOCKED >3s] Thread '%s' (TID %lu) waiting on %s (res 0x%" PRIx64 ") for %.1fs\n",
                                t->name, (unsigned long)t->win32_tid, t->blocked_on, t->blocked_resource, wait_s);
                    }
                    if (f_heartbeat_ts) {
                        fprintf(f_heartbeat_ts, "  [BLOCKED >3s] Thread '%s' (TID %lu) waiting on %s (res 0x%" PRIx64 ") for %.1fs\n",
                                t->name, (unsigned long)t->win32_tid, t->blocked_on, t->blocked_resource, wait_s);
                    }
                }
            }

            if (tlock) pthread_mutex_unlock(tlock);

            fflush(stdout);
            if (f_heartbeat) fflush(f_heartbeat);
            if (f_heartbeat_ts) fflush(f_heartbeat_ts);
            if (g_import_file) fflush(g_import_file);

            if (g_last_progress_tick != 0 && (now - g_last_progress_tick >= 10000)) {
                if (now - last_dump_tick >= 10000) {
                    last_dump_tick = now;
                    dump_watchdog();
                }
            }
        }
        if (elapsed_s > 0 && (elapsed_s % 10 == 0)) {
            double avg_fps = 0.0, p95_ms = 0.0, p99_ms = 0.0;
            if (gpu_enabled) {
                bbgpu_get_frametime_percentiles(&avg_fps, &p95_ms, &p99_ms);
            }
            if (avg_fps <= 0.0 && elapsed_s > 0) {
                avg_fps = (double)cur_flips / (double)elapsed_s;
            }
            uint64_t audio_underruns = runtime_audio_get_underruns();
            FILE *f_sum = fopen("out/summary.log", "w");
            if (f_sum) {
                fprintf(f_sum, "=== BLOODBORNE 10S SUMMARY ===\n");
                fprintf(f_sum, "Elapsed:         %llu s\n", (unsigned long long)elapsed_s);
                fprintf(f_sum, "Total Flips:     %llu\n", (unsigned long long)cur_flips);
                fprintf(f_sum, "Average Flips/s: %.2f\n", avg_fps);
                fprintf(f_sum, "p95 Frame Time:  %.2f ms\n", p95_ms);
                fprintf(f_sum, "p99 Frame Time:  %.2f ms\n", p99_ms);
                fprintf(f_sum, "Audio Underruns: %llu\n", (unsigned long long)audio_underruns);
                fflush(f_sum);
                fclose(f_sum);
            }
        }
    }
    if (f_heartbeat) fclose(f_heartbeat);
    if (f_heartbeat_ts) fclose(f_heartbeat_ts);
    return 0;
}

static void start_watchdog(void) {
    const char *env_wd = getenv("BB_WATCHDOG");
    if (env_wd && (!strcmp(env_wd, "0") || !strcmp(env_wd, "off") || !strcmp(env_wd, "false"))) {
        printf("[WATCHDOG] Watchdog thread disabled (BB_WATCHDOG=0).\n");
        return;
    }
    g_last_progress_tick = GetTickCount64();
    CreateThread(NULL, 0, watchdog_worker, NULL, 0, NULL);
}

static DWORD WINAPI timeout_worker(LPVOID param) {
    DWORD sec = (DWORD)(uintptr_t)param;
    if (sec > 0) {
        Sleep(sec * 1000);
        printf("\n[TIMEOUT] Reached timeout of %lu seconds, terminating cleanly.\n", (unsigned long)sec);
        fflush(stdout);
        print_exit_summary();
        /* Not ExitProcess: its DLL detach (GPU driver, audio) can hang with the game's threads
         * killed mid-operation, leaving the process alive with its memory. */
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return 0;
}
static void start_timeout(unsigned seconds) {
    if (seconds > 0) {
        CreateThread(NULL, 0, timeout_worker, (LPVOID)(uintptr_t)seconds, 0, NULL);
    }
}
#else
static void start_watchdog(void) {}
static void start_timeout(unsigned seconds) {
    alarm(seconds);
}
#endif
#ifdef _WIN32
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(
".text\n"
".globl enter_on_stack\n"
"enter_on_stack:\n"
"   push %rbp\n"
"   mov %rsp, %rbp\n"
"   push %rbx\n"
"   push %r12\n"
"   push %r13\n"
"   push %r14\n"
"   push %r15\n"
"   mov %gs:0x8, %r12\n"
"   mov %gs:0x10, %r13\n"
"   mov %rcx, %r14\n" /* save entry */
"   mov %rdx, %r15\n" /* save arg0 */
"   mov %r8, %rbx\n"  /* save arg1: caller shadow space is clobbered by the calls below */
"   mov %r9, %rsp\n"
"   and $-16, %rsp\n"
"   mov %rsp, %gs:0x8\n"
"   sub $0x800000, %r9\n"
"   mov %r9, %gs:0x10\n"
"   sub $0x20, %rsp\n"
"   call runtime_win_has_fsgsbase\n"
"   test %eax, %eax\n"
"   jz 1f\n"
"   call runtime_thread_get_tcb\n"
"   test %rax, %rax\n"
"   jz 1f\n"
"   wrfsbase %rax\n"
"1:\n"
"   mov %r14, %r11\n"
"   mov %r15, %rdi\n"
"   mov %rbx, %rsi\n"
"   call *%r11\n"
"   add $0x20, %rsp\n"
"   mov %r12, %gs:0x8\n"
"   mov %r13, %gs:0x10\n"
"   lea -0x28(%rbp), %rsp\n"
"   pop %r15\n"
"   pop %r14\n"
"   pop %r13\n"
"   pop %r12\n"
"   pop %rbx\n"
"   pop %rbp\n"
"   ret\n"
);
static void veh_write(const char *msg) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != NULL) {
        DWORD written;
        WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
    }
}

/* A fatal exception must end the process even when its handler blocks (a lock held by a
 * crashed thread in the GPU dump, a full pipe to the launcher): bbport.exe then stayed alive
 * with gigabytes of RAM and VRAM. This thread exists before the game runs, so arming it takes
 * no lock: it terminates the process a few seconds after the first fatal exception. */
static HANDLE g_crash_event;
static volatile LONG g_crash_thread;
/* Classification of the fatal path, published before the dump so that the reaper still exits
 * with the right code if the dump blocks. 0 means "not classified" (generic 139). */
static volatile LONG g_crash_exit_code;
static int g_crash_test_hang;

static DWORD WINAPI crash_reaper(LPVOID param) {
    (void)param;
    WaitForSingleObject(g_crash_event, INFINITE);
    Sleep(5000);
    LONG code = InterlockedCompareExchange(&g_crash_exit_code, 0, 0);
    TerminateProcess(GetCurrentProcess(), (UINT)(code ? code : 128 + 11));
    return 0;
}

/* First step of every fatal path. Another thread faulting meanwhile waits for the first
 * dump instead of interleaving its own; the reaper ends both. */
static void crash_begin(void) {
    if (g_crash_event) SetEvent(g_crash_event);
    const LONG self = (LONG)GetCurrentThreadId();
    const LONG owner = InterlockedCompareExchange(&g_crash_thread, self, 0);
    if (owner != 0 && owner != self) Sleep(INFINITE);
}

/* BB_CRASH_TEST=fault|hang[:seconds] exercises the crash path: a host thread faults after
 * the delay (20 s by default), and with "hang" the handler then blocks the way a stuck dump
 * would. */
static DWORD WINAPI crash_test_worker(LPVOID param) {
    Sleep((DWORD)(uintptr_t)param);
    *(volatile int *)(uintptr_t)8 = 0;
    return 0;
}

static void start_crash_reaper(void) {
    g_crash_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_crash_event) CreateThread(NULL, 0, crash_reaper, NULL, 0, NULL);
    const char *test = getenv("BB_CRASH_TEST");
    if (test && *test) {
        g_crash_test_hang = !strncmp(test, "hang", 4);
        const char *delay = strchr(test, ':');
        const unsigned long seconds = delay ? strtoul(delay + 1, NULL, 10) : 20;
        printf("[CRASH TEST] host fault in %lu s%s\n", seconds,
               g_crash_test_hang ? ", then a blocked crash handler" : "");
        CreateThread(NULL, 0, crash_test_worker, (LPVOID)(uintptr_t)(seconds * 1000), 0, NULL);
    }
}

#ifdef _WIN32
static void resolve_host_address(uintptr_t addr, char *out_buf, size_t out_buf_sz) {
    if (!addr) {
        snprintf(out_buf, out_buf_sz, "NULL");
        return;
    }
    if (addr < 0x10000) {
        /* A near-NULL value: 0x10/0x40/... are offsets into a struct whose base was NULL,
         * not host addresses. */
        snprintf(out_buf, out_buf_sz, "NULL+0x%" PRIxPTR, addr);
        return;
    }
    HMODULE hMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod) && hMod) {
        char mod_path[MAX_PATH] = {0};
        if (GetModuleFileNameA(hMod, mod_path, sizeof(mod_path))) {
            const char *base = strrchr(mod_path, '\\');
            if (!base) base = strrchr(mod_path, '/');
            base = base ? (base + 1) : mod_path;
            uintptr_t offset = addr - (uintptr_t)hMod;
            snprintf(out_buf, out_buf_sz, "%s +0x%" PRIxPTR " [base %p]", base, offset, (void *)hMod);
            return;
        }
    }
    snprintf(out_buf, out_buf_sz, "non-module 0x%" PRIxPTR, addr);
}

/* Full address classification for stack/heap dumps: guest code, guest stack/heap (the low
 * mapping range used by runtime_low_map), a real host module, or a near-NULL value. */
static void describe_pointer(uintptr_t val, char *out, size_t out_sz) {
    if (!val) { snprintf(out, out_sz, "NULL"); return; }
    if (val < 0x10000) { snprintf(out, out_sz, "NULL+0x%" PRIxPTR, val); return; }
    if (is_guest_address(val)) {
        uintptr_t off = val - (uintptr_t)image;
        for (uint64_t m = 0; m < module_count; ++m) {
            if (off >= modules[m].base && off < modules[m].base + modules[m].size) {
                snprintf(out, out_sz, "guest module %" PRIu64 " +0x%" PRIxPTR, m, off - modules[m].base);
                return;
            }
        }
        snprintf(out, out_sz, "guest eboot.bin +0x%" PRIxPTR, off);
        return;
    }
    if (val >= 0x0800000000ull && val < 0x1000000000ull) {
        snprintf(out, out_sz, "guest stack/heap %p", (void *)val);
        return;
    }
    resolve_host_address(val, out, out_sz);
}

/* Decode at most 16 bytes at addr with Zydis. The caller must have verified that the range is
 * readable (Zydis reads directly; an unmapped page would fault the crash handler itself). */
static int decode_instruction_text(uintptr_t addr, char *out, size_t out_sz) {
    if (!addr || IsBadReadPtr((const void *)addr, 16)) return 0;
    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return 0;
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, (const void *)addr, 16, &insn, operands)))
        return 0;
    ZydisFormatter formatter;
    if (ZYAN_FAILED(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)))
        return 0;
    if (ZYAN_FAILED(ZydisFormatterFormatInstruction(&formatter, &insn, operands,
                                                    insn.operand_count_visible, out, out_sz,
                                                    (ZyanU64)addr, NULL)))
        return 0;
    return 1;
}

/* Copy a guest string for diagnostics. Reads byte-by-byte with an accessibility check: an
 * assert's format string can itself be the NULL/garbage pointer that caused the abort. */
static void copy_guest_string(uintptr_t ptr, char *out, size_t out_sz) {
    if (!out_sz) return;
    out[0] = 0;
    if (!ptr) { snprintf(out, out_sz, "<NULL>"); return; }
    if (IsBadReadPtr((const void *)ptr, 1)) { snprintf(out, out_sz, "<%p unreadable>", (void *)ptr); return; }
    size_t i = 0;
    while (i + 1 < out_sz && !IsBadReadPtr((const void *)(ptr + i), 1)) {
        char ch = ((const char *)ptr)[i];
        if (!ch) break;
        out[i++] = (ch == '\n' || ch == '\t' || (ch >= 0x20 && ch < 0x7f)) ? ch : '.';
    }
    out[i] = 0;
}

/* The game's panic/assert printer at image+0x20b55b0 is called with
 *   rdi = first string (source file/expression), esi = assert id, rdx = second string,
 *   al  = number of vector registers used by the variadic arguments.
 * A stub at the entry records these before the printer decides to print, return silently
 * (mode 2) or write to address 0 and abort. */
static volatile LONG g_assert_hook_calls;
static volatile uint64_t g_last_assert_id, g_last_assert_s1, g_last_assert_s2;

static void guest_assert_hook(uint64_t s1, uint64_t id, uint64_t s2) {
    LONG seen = InterlockedIncrement(&g_assert_hook_calls);
    g_last_assert_id = id;
    g_last_assert_s1 = s1;
    g_last_assert_s2 = s2;
    if (seen > 20) return; /* keep the first 20; the dump always has the latest */
    char a[256], b[512];
    copy_guest_string((uintptr_t)s1, a, sizeof(a));
    copy_guest_string((uintptr_t)s2, b, sizeof(b));
    fprintf(stderr, "[GAME ASSERT] id=0x%llx  \"%s\"  \"%s\"  (s1=%p s2=%p, thread '%s' TID %lu)\n",
            (unsigned long long)id, a, b, (void *)(uintptr_t)s1, (void *)(uintptr_t)s2,
            runtime_thread_current() ? runtime_thread_current()->name : "none",
            (unsigned long)GetCurrentThreadId());
    if (seen == 20) fprintf(stderr, "[GAME ASSERT] further assertions are recorded in the crash dump only\n");
    fflush(stderr);
}

static void guest_assert_write_last(FILE *f) {
    if (!g_assert_hook_calls) return;
    char a[256], b[512];
    copy_guest_string((uintptr_t)g_last_assert_s1, a, sizeof(a));
    copy_guest_string((uintptr_t)g_last_assert_s2, b, sizeof(b));
    fprintf(f, "\n--- LAST GUEST ASSERTION (captured at 0x20b55b0) ---\n");
    fprintf(f, "Assert id: 0x%llx   (assert printer calls this run: %ld)\n",
            (unsigned long long)g_last_assert_id, (long)g_assert_hook_calls);
    fprintf(f, "String 1:  \"%s\"  [%p]\n", a, (void *)(uintptr_t)g_last_assert_s1);
    fprintf(f, "String 2:  \"%s\"  [%p]\n", b, (void *)(uintptr_t)g_last_assert_s2);
}

static void dump_loaded_modules(FILE *f) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    fprintf(f, "\n--- LOADED MODULES ---\n");
    if (Module32First(snap, &me)) {
        do {
            uintptr_t base = (uintptr_t)me.modBaseAddr;
            uintptr_t end = base + me.modBaseSize;
            fprintf(f, "  [0x%016llx - 0x%016llx] %-28s (size: 0x%08x, path: %s)\n",
                    (unsigned long long)base, (unsigned long long)end,
                    me.szModule, (unsigned int)me.modBaseSize, me.szExePath);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}
#else
static void resolve_host_address(uintptr_t addr, char *out_buf, size_t out_buf_sz) {
    snprintf(out_buf, out_buf_sz, "host %p", (void *)addr);
}
static void dump_loaded_modules(FILE *f) { (void)f; }
#endif

static const char *g_crash_dump_path_override = NULL;
static const char *g_crash_dump_note = NULL;
static int g_crash_dump_append = 0;
/* Set for a guest assert that continues execution: GPU queries can take a lock the asserting
 * thread may already hold, so they are skipped (a fatal crash may query them). */
static int g_crash_dump_guest_assert = 0;

static void write_crash_dump(EXCEPTION_POINTERS *ep, void *fault_addr, uintptr_t rip, DWORD code) {
    char crash_path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("out", NULL);
    if (g_crash_dump_path_override && g_crash_dump_path_override[0]) {
        snprintf(crash_path, sizeof(crash_path), "%s", g_crash_dump_path_override);
    } else {
        snprintf(crash_path, sizeof(crash_path), "out/crash_%04d%02d%02d_%02d%02d%02d.log",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    FILE *f = fopen(crash_path, g_crash_dump_append ? "a" : "w");
    if (!f) f = fopen("crash.log", g_crash_dump_append ? "a" : "w");
    if (!f) return;

    const int guest_rip = is_guest_address(rip);
    uintptr_t off = guest_rip ? (rip - (uintptr_t)image) : 0;
    fprintf(f, "================================================================================\n");
    fprintf(f, "=== BLOODBORNE CRASH DUMP ===\n");
    fprintf(f, "================================================================================\n\n");
    /* Build identity: a dump that does not name its binary cannot be matched to a commit or a
     * build, and a reporter's binary is usually not the one built locally (the reported RVAs
     * in the C5/C8 crashes matched no local build). The PE fields are the ones dumpbin
     * /headers prints, so the dump alone identifies the image; __DATE__/__TIME__ is this
     * translation unit's compile stamp. Header reads only: no allocation, VEH-safe. */
    fprintf(f, "Port build:     %s %s (probe.c compiled)\n", __DATE__, __TIME__);
#ifdef _WIN32
    {
        char module_path[MAX_PATH] = "(unavailable)";
        HMODULE module = GetModuleHandleA(NULL);
        if (module && GetModuleFileNameA(module, module_path, (DWORD)sizeof(module_path)))
            fprintf(f, "Host module:    %s [base %p]\n", module_path, (void *)module);
        else
            fprintf(f, "Host module:    (unavailable)\n");
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
        const IMAGE_NT_HEADERS *nt = NULL;
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
            nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
        if (nt && nt->Signature == IMAGE_NT_SIGNATURE)
            fprintf(f, "Build ID:       TimeDateStamp 0x%08lx SizeOfImage 0x%08lx CheckSum 0x%08lx\n",
                    (unsigned long)nt->FileHeader.TimeDateStamp,
                    (unsigned long)nt->OptionalHeader.SizeOfImage,
                    (unsigned long)nt->OptionalHeader.CheckSum);
        else
            fprintf(f, "Build ID:       (no PE header at the host module)\n");
    }
#endif
    fprintf(f, "Exception Code: 0x%08lx\n", code);
    if (g_crash_dump_note && g_crash_dump_note[0]) fprintf(f, "%s\n", g_crash_dump_note);
    char fault_desc[256];
    describe_pointer((uintptr_t)fault_addr, fault_desc, sizeof(fault_desc));
    const char *addr_label = g_crash_dump_guest_assert ? "Raising site" : "Fault Address";
#ifdef _WIN32
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR access_type = ep->ExceptionRecord->ExceptionInformation[0];
        const char *type_str = (access_type == 0) ? "READ" : (access_type == 1) ? "WRITE" : (access_type == 8) ? "EXECUTE (DEP)" : "UNKNOWN";
        fprintf(f, "%s:  %p (%s violation) [%s]\n", addr_label, fault_addr, type_str, fault_desc);
    } else {
        fprintf(f, "%s:  %p [%s]\n", addr_label, fault_addr, fault_desc);
    }
#else
    fprintf(f, "%s:  %p [%s]\n", addr_label, fault_addr, fault_desc);
#endif

    char rip_resolved[512];
    if (guest_rip)
        snprintf(rip_resolved, sizeof(rip_resolved), "guest eboot.bin offset 0x%" PRIxPTR, off);
    else
        resolve_host_address(rip, rip_resolved, sizeof(rip_resolved));
    fprintf(f, "Faulting RIP:   %p (%s)\n", (void *)rip, rip_resolved);

    /* The instruction itself, decoded: a raw register/stack dump cannot tell a NULL write
     * from a mis-decoded guest trap. Zydis reads at most 16 bytes; the range is checked. */
    fprintf(f, "\n--- INSTRUCTION AT RIP ---\n");
    if (rip && !IsBadReadPtr((const void *)rip, 16)) {
        fprintf(f, "  Bytes:");
        for (int i = 0; i < 16; ++i) fprintf(f, " %02x", ((const unsigned char *)rip)[i]);
        fprintf(f, "\n");
        char text[256];
        if (decode_instruction_text(rip, text, sizeof(text))) fprintf(f, "  %s\n\n", text);
        else fprintf(f, "  (instruction not decoded)\n\n");
    } else {
        fprintf(f, "  (RIP is not readable)\n\n");
    }

    CONTEXT *c = ep->ContextRecord;
    fprintf(f, "--- REGISTERS ---\n");
    fprintf(f, "RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
            (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
    fprintf(f, "RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
            (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    fprintf(f, "R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
            (unsigned long long)c->R8,  (unsigned long long)c->R9,  (unsigned long long)c->R10, (unsigned long long)c->R11);
    fprintf(f, "R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)c->R12, (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15);
    void *cur_fs = runtime_win_read_fsbase();
    fprintf(f, "RIP=%016llx EFLAGS=%08lx FS_BASE=%p GS_SELECTOR=0x%x\n\n",
            (unsigned long long)c->Rip, (unsigned long)c->EFlags, cur_fs, (unsigned int)c->SegGs);

    fprintf(f, "\n--- HOST STACK (unwound from this handler) ---\n");
    {
        /* A real unwind, not a raw qword dump. The faulting RIP is guest code (no unwind
         * data), so this only reaches the host frames below enter_on_stack: still the only
         * reliable way to see which host path dispatched into the guest. */
        void *frames[32];
        USHORT count = RtlCaptureStackBackTrace(0, 32, frames, NULL);
        if (!count) fprintf(f, "  (no host frames)\n");
        for (USHORT i = 0; i < count; ++i) {
            char loc[512];
            describe_pointer((uintptr_t)frames[i], loc, sizeof(loc));
            fprintf(f, "  [host %2u] %p (%s)\n", (unsigned)i, frames[i], loc);
        }
    }

    /* enter_on_stack repoints the thread's TEB stack bounds at the guest stack, so during
     * guest execution these are the *guest* bounds (and the RBP chain below is in range). */
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const uintptr_t stack_low = (uintptr_t)tib->StackLimit;
    const uintptr_t stack_high = (uintptr_t)tib->StackBase;
    const int context_is_guest = is_guest_address(c->Rip);

    fprintf(f, "\n--- %sRBP FRAME CHAIN ---\n", context_is_guest ? "GUEST " : "HOST ");
    {
        uintptr_t rbp = c->Rbp;
        int printed = 0;
        int valid = rbp >= stack_low && rbp < stack_high && !(rbp & 7);
        if (!valid)
            fprintf(f, "  (RBP %p outside thread stack [%p, %p))\n", (void *)rbp,
                    (void *)stack_low, (void *)stack_high);
        for (int frame = 0; frame < 24 && valid; ++frame) {
            if (rbp + 16 > stack_high || IsBadReadPtr((const void *)rbp, 16)) break;
            const uintptr_t *fp = (const uintptr_t *)rbp;
            uintptr_t next_rbp = fp[0];
            uintptr_t ret_addr = fp[1];
            if (!ret_addr) break;
            char loc[512];
            describe_pointer(ret_addr, loc, sizeof(loc));
            fprintf(f, "  [rbp %2d] ret=%016llx (%s)\n", frame, (unsigned long long)ret_addr, loc);
            ++printed;
            /* A frame pointer chain must move towards the stack base in aligned steps. A
             * single stale/garbage RBP would otherwise walk into arbitrary memory. */
            if (next_rbp <= rbp || next_rbp >= stack_high || (next_rbp & 7)) break;
            rbp = next_rbp;
        }
        if (!printed)
            fprintf(f, "  (no valid %s frame chain)\n", context_is_guest ? "guest" : "host");
    }

    fprintf(f, "\n--- GUEST CODE POINTERS ON THE STACK (scan) ---\n");
    {
        /* Guest code has no unwind data, so scan a bounded window of the stack for values in
         * the loaded image. This is only a candidate list (data pointers share the range), but
         * it recovers the caller chain when RBP is not a frame pointer. */
        uintptr_t rsp = c->Rsp;
        int shown = 0;
        if (rsp >= stack_low && rsp < stack_high) {
            uintptr_t end = rsp + 0x8000;
            if (end > stack_high) end = stack_high;
            for (uintptr_t p = rsp; p + 8 <= end && shown < 24; p += 8) {
                if (IsBadReadPtr((const void *)p, 8)) break;
                uintptr_t val = *(const uintptr_t *)p;
                if (!is_guest_address(val)) continue;
                char loc[512];
                describe_pointer(val, loc, sizeof(loc));
                fprintf(f, "  [rsp+0x%04" PRIxPTR "] %016" PRIxPTR " (%s)\n",
                        p - rsp, val, loc);
                ++shown;
            }
        }
        if (!shown) fprintf(f, "  (none found)\n");
    }

    fprintf(f, "\n--- LAST 32 IMPORTS OF FAULTING THREAD ---\n");
    GuestThread *cur = runtime_thread_current();
    if (cur) {
        fprintf(f, "Thread: '%s' (TID %lu, total thread imports: %u)\n", cur->name, (unsigned long)cur->win32_tid, cur->recent_import_count);
        uint32_t count = cur->recent_import_count < 32 ? cur->recent_import_count : 32;
        uint32_t start = cur->recent_import_count > 32 ? (cur->recent_import_count - 32) : 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = cur->recent_imports[(start + i) & 31];
            const char *name = (idx < import_count) ? names[idx] : "unknown";
            const char *sym = runtime_import_name(name);
            fprintf(f, "  #%2u: idx %4u - %s (%s)\n", i + 1, idx, name, sym ? sym : "unknown");
        }
    } else {
        fprintf(f, "  (No guest thread context available)\n");
    }

    runtime_memory_dump_recent_ops(f);
    if (gpu_enabled && !g_crash_dump_guest_assert) bbgpu_dump_breadcrumbs(f);

    char gpu_vendor[128] = "Unknown Vendor";
    char gpu_model[256] = "Unknown GPU";
    char gpu_driver[128] = "Unknown Driver";
    char gpu_api[64] = "Unknown API";
    if (gpu_enabled && !g_crash_dump_guest_assert) {
        bbgpu_get_device_info(gpu_vendor, sizeof(gpu_vendor),
                              gpu_model, sizeof(gpu_model),
                              gpu_driver, sizeof(gpu_driver),
                              gpu_api, sizeof(gpu_api));
    }
    fprintf(f, "\n--- GPU & DRIVER TELEMETRY ---\n");
    if (g_crash_dump_guest_assert) {
        fprintf(f, "GPU State:     skipped for a continuing assert (the asserting thread may hold a GPU lock)\n");
    } else if (gpu_enabled && strcmp(gpu_model, "Unknown GPU") != 0) {
        fprintf(f, "GPU Model:     %s\n", gpu_model);
        fprintf(f, "GPU Vendor:    %s\n", gpu_vendor);
        fprintf(f, "Driver:        %s\n", gpu_driver);
        fprintf(f, "Vulkan API:    %s\n", gpu_api);
    } else {
        fprintf(f, "GPU State:     Not initialized / Headless\n");
    }

    dump_loaded_modules(f);

    fprintf(f, "\n--- HOST MEMORY ---\n");
    {
        MEMORYSTATUSEX ms;
        memset(&ms, 0, sizeof(ms));
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            fprintf(f, "Physical: %llu MiB total, %llu MiB available\n",
                    (unsigned long long)(ms.ullTotalPhys >> 20),
                    (unsigned long long)(ms.ullAvailPhys >> 20));
            fprintf(f, "Commit:   %llu MiB in use of %llu MiB limit, %llu MiB available\n",
                    (unsigned long long)((ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20),
                    (unsigned long long)(ms.ullTotalPageFile >> 20),
                    (unsigned long long)(ms.ullAvailPageFile >> 20));
        } else {
            fprintf(f, "  (GlobalMemoryStatusEx failed, error %lu)\n", (unsigned long)GetLastError());
        }
    }

    runtime_memory_report_to(f);
    guest_assert_write_last(f);

    fflush(f);
    fclose(f);
    char note[256];
    snprintf(note, sizeof(note), "Crash details written to %s\n", crash_path);
    veh_write(note);
}

/* Assert/diagnostic dump, called from raise_exception() for guest debug exceptions. The game
 * can assert repeatedly (issue #148 did); the first 50 assertions get their own timestamped
 * full dump, later ones append a compact record to one file instead of creating thousands of
 * files or repeating the expensive dump. Re-entrancy is guarded because the dump path itself
 * could assert. */
static volatile LONG g_assert_dump_active = 0;
static volatile LONG g_assert_dump_count = 0;
static char g_assert_dump_repeat[MAX_PATH];

const char *runtime_crash_dump_assert(uint32_t code, uint64_t argument, uint64_t guest_offset) {
    static char last_path[MAX_PATH];
    if (InterlockedCompareExchange(&g_assert_dump_active, 1, 0) != 0) return NULL;

    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("out", NULL);
    LONG count = InterlockedIncrement(&g_assert_dump_count);
    if (count <= 50) {
        snprintf(last_path, sizeof(last_path), "out/assert_%04d%02d%02d_%02d%02d%02d_%ld.log",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (long)count);
    } else {
        if (!g_assert_dump_repeat[0])
            snprintf(g_assert_dump_repeat, sizeof(g_assert_dump_repeat), "out/assert_repeat.log");
        snprintf(last_path, sizeof(last_path), "%s", g_assert_dump_repeat);
        /* A per-frame assert must not pay for a full context/stack/GPU dump every time. */
        FILE *rf = fopen(g_assert_dump_repeat, "a");
        if (rf) {
            char s1[192], s2[384];
            copy_guest_string((uintptr_t)g_last_assert_s1, s1, sizeof(s1));
            copy_guest_string((uintptr_t)g_last_assert_s2, s2, sizeof(s2));
            fprintf(rf, "[%04d-%02d-%02d %02d:%02d:%02d] code=0x%x argument=0x%llx guest=0x%llx "
                        "assert=0x%llx s1=\"%s\" s2=\"%s\"\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    code, (unsigned long long)argument, (unsigned long long)guest_offset,
                    (unsigned long long)g_last_assert_id, s1, s2);
            if (count == 51)
                fprintf(rf, "(further assertions use this compact form; full dumps are capped at 50)\n");
            fclose(rf);
        }
        InterlockedExchange(&g_assert_dump_active, 0);
        return last_path;
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);

    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = 0xE0000001; /* application-defined: guest assertion */
    record.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
    EXCEPTION_POINTERS pointers;
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &ctx;

    char note[768];
    snprintf(note, sizeof(note),
             "Guest debug exception: code=0x%x argument=0x%llx\n"
             "Raising guest offset: 0x%llx%s\n"
             "(context below is the host state inside the runtime; the guest registers are in\n"
             " the thread's last import record and the guest stack scan)",
             code, (unsigned long long)argument, (unsigned long long)guest_offset,
             guest_offset ? "" : " (site not found on the stack)");
    void *site = guest_offset ? (void *)((uintptr_t)image + guest_offset) : NULL;

    g_crash_dump_path_override = last_path;
    g_crash_dump_append = 0;
    g_crash_dump_note = note;
    g_crash_dump_guest_assert = 1;
    write_crash_dump(&pointers, site, ctx.Rip, code);
    g_crash_dump_guest_assert = 0;
    g_crash_dump_note = NULL;
    g_crash_dump_path_override = NULL;
    g_crash_dump_append = 0;

    InterlockedExchange(&g_assert_dump_active, 0);
    return last_path;
}

#ifdef _WIN32
typedef struct {
    uintptr_t addr;
    uint8_t len;
    uint8_t orig[10];
} LzcntPatchSite;

#define MAX_LZCNT_SITES 2048
static LzcntPatchSite s_lzcnt_sites[MAX_LZCNT_SITES];
static uint32_t s_num_lzcnt_sites = 0;

static int compare_lzcnt_sites(const void *a, const void *b) {
    uintptr_t addr_a = ((const LzcntPatchSite *)a)->addr;
    uintptr_t addr_b = ((const LzcntPatchSite *)b)->addr;
    if (addr_a < addr_b) return -1;
    if (addr_a > addr_b) return 1;
    return 0;
}

static const unsigned char NOP_PATTERNS[10][10] = {
    {0},
    {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}
};

static void fill_nops(unsigned char *dst, size_t count) {
    while (count > 0) {
        size_t take = count > 9 ? 9 : count;
        memcpy(dst, NOP_PATTERNS[take], take);
        dst += take;
        count -= take;
    }
}

static int emulate_lzcnt_site(CONTEXT *ctx, uintptr_t rip) {
    if (s_num_lzcnt_sites == 0) return 0;
    LzcntPatchSite key;
    key.addr = rip;
    LzcntPatchSite *site = (LzcntPatchSite *)bsearch(&key, s_lzcnt_sites, s_num_lzcnt_sites, sizeof(LzcntPatchSite), compare_lzcnt_sites);
    if (!site) return 0;
    /* Refuse a site whose recorded encoding is impossible; advancing by a wrong length would
     * resume in the middle of the next instruction. */
    if (site->len < 4 || site->len > 10) return 0;

    DWORD64 *regs[16] = {
        &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
        &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
        &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
        &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15
    };

    /* Encodings in the linked image include redundant/size prefixes (F3 F3, 66 F3, 49 F3 49,
     * 47 F3 48, ...), so the instruction does not necessarily start with F3. Skip legacy
     * prefixes and REX (the last REX before the opcode wins), then require 0F BD. */
    const unsigned char *b = site->orig;
    int idx = 0, rex = 0, opsize = 4;
    for (; idx + 2 < site->len; ++idx) {
        unsigned char c = b[idx];
        if (c == 0x0F) break;
        if ((c & 0xF0) == 0x40) { rex = c; continue; }
        if (c == 0x66) { opsize = 2; continue; }
        if (c == 0x67 || c == 0xF0 || c == 0xF2 || c == 0xF3 ||
            c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 || c == 0x65)
            continue;
        return 0; /* not a prefix: not the encoding that was recorded */
    }
    if (idx + 2 >= site->len || b[idx] != 0x0F || b[idx + 1] != 0xBD) return 0;
    if ((rex >> 3) & 1) opsize = 8; /* REX.W overrides a 66 prefix */
    idx += 2; /* skip 0F BD */
    unsigned char modrm = b[idx++];
    int r_bit = rex ? ((rex >> 2) & 1) : 0;
    int x_bit = rex ? ((rex >> 1) & 1) : 0;
    int b_bit = rex ? (rex & 1) : 0;
    int dest_idx = ((modrm >> 3) & 7) | (r_bit << 3);
    int mod = modrm >> 6;
    int rm = modrm & 7;
    int src_idx = rm | (b_bit << 3);

    uint64_t src_val = 0;
    if (mod == 3) {
        src_val = *regs[src_idx];
    } else {
        uintptr_t ea = 0;
        const unsigned char *cur = b + idx;
        int next_len = site->len;
        if (rm == 4) {
            unsigned char sib = *cur++;
            int scale = 1 << (sib >> 6);
            int index_reg = ((sib >> 3) & 7) | (x_bit << 3);
            int sib_base = (sib & 7) | (b_bit << 3);
            uint64_t base_val = 0;
            if (mod == 0 && (sib & 7) == 5) {
                int32_t disp = *(const int32_t *)cur;
                cur += 4;
                base_val = (uint64_t)(int64_t)disp;
            } else {
                base_val = *regs[sib_base];
            }
            uint64_t idx_val = (index_reg != 4) ? *regs[index_reg] : 0;
            ea = base_val + idx_val * scale;
        } else if (mod == 0 && rm == 5) {
            int32_t disp = *(const int32_t *)cur;
            cur += 4;
            ea = (rip + next_len) + disp;
        } else {
            ea = *regs[src_idx];
        }
        if (mod == 1) {
            int8_t disp = *(const int8_t *)cur++;
            ea += disp;
        } else if (mod == 2) {
            int32_t disp = *(const int32_t *)cur;
            cur += 4;
            ea += disp;
        }
        if (IsBadReadPtr((const void *)ea, (size_t)opsize)) return 0;
        src_val = (opsize == 8) ? *(const uint64_t *)ea :
                  (opsize == 4) ? (uint64_t)*(const uint32_t *)ea :
                                  (uint64_t)*(const uint16_t *)ea;
    }

    unsigned bits = (unsigned)opsize * 8;
    uint64_t mask = (opsize == 8) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - 1);
    src_val &= mask;

    /* LZCNT clears OF/SF/AF/PF; CF and ZF carry the result. The old emulation left the
     * caller's OF/SF/AF/PF in place, unlike the instruction it replaces. */
    ctx->EFlags &= ~(0x800 | 0x80 | 0x10 | 0x04);

    uint64_t res;
    if (src_val == 0) {
        res = bits;
        ctx->EFlags |= 0x01;  /* CF = 1 */
        ctx->EFlags &= ~0x40; /* ZF = 0 */
    } else {
        unsigned long bit_idx = 0;
        _BitScanReverse64(&bit_idx, src_val);
        res = (uint64_t)bits - 1 - (uint64_t)bit_idx;
        ctx->EFlags &= ~0x01; /* CF = 0 */
        if (res == 0) ctx->EFlags |= 0x40;  /* ZF = 1 */
        else          ctx->EFlags &= ~0x40; /* ZF = 0 */
    }
    if (opsize == 2)
        *regs[dest_idx] = (*regs[dest_idx] & ~UINT64_C(0xFFFF)) | res; /* 16-bit write keeps the upper bits */
    else
        *regs[dest_idx] = res; /* a 32-bit write zero-extends; a 64-bit write is full width */
    ctx->Rip += site->len;
    return 1;
}

/* Replace every decoded LZCNT with UD2 and remember its exact length. The previous byte scan
 * assumed LZCNT was always `F3 [REX] 0F BD modrm` with no displacement/SIB and patched only
 * the first two bytes, leaving the rest of the instruction to execute as garbage. The linked
 * image has 541 raw F3/0F BD patterns: 406 real LZCNT instructions (some with extra prefixes)
 * and 135 that are bytes inside other instructions. A linear Zydis sweep derives the true
 * length and only real LZCNT instructions are patched; raw patterns inside other instructions
 * are data and are reported instead of patched. */
static void patch_non_bmi1(unsigned char *img, uint64_t total_size, Segment *segs, uint64_t num_segs) {
    uint32_t patched = 0, skipped = 0;

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        fprintf(stderr, "Non-BMI1 CPU: Zydis decoder init failed; LZCNT traps were NOT installed\n");
        return;
    }

    for (uint64_t s = 0; s < num_segs; ++s) {
        if (!(segs[s].flags & 1)) continue; /* executable segments only */
        uint64_t seg_start = segs[s].address;
        uint64_t seg_end = seg_start + segs[s].size;
        if (seg_end > total_size) seg_end = total_size;
        if (seg_end <= seg_start) continue;

        uint64_t pos = seg_start;
        while (pos < seg_end) {
            ZydisDecodedInstruction insn;
            if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(&decoder, NULL, img + pos, seg_end - pos, &insn))) {
                ++pos; /* invalid byte: resynchronise at the next one */
                continue;
            }
            uint8_t len = insn.length;
            if (len < 4 || len > 10 || pos + len > seg_end) {
                ++pos;
                continue;
            }
            if (insn.mnemonic == ZYDIS_MNEMONIC_LZCNT) {
                if (s_num_lzcnt_sites < MAX_LZCNT_SITES) {
                    LzcntPatchSite *site = &s_lzcnt_sites[s_num_lzcnt_sites++];
                    site->addr = (uintptr_t)(img + pos);
                    site->len = len;
                    memcpy(site->orig, img + pos, len);
                    img[pos] = 0x0F;
                    img[pos + 1] = 0x0B;
                    ++patched;
                } else {
                    ++skipped;
                }
            }
            pos += len;
        }
    }

    /* Anything that still looks like `F3 [REX] 0F BD` was not reached by the sweep, i.e. it is
     * not on a decoded instruction boundary. Patching such a byte would corrupt data or split a
     * different instruction, so leave it and say so. */
    uint32_t unconfirmed = 0;
    for (uint64_t s = 0; s < num_segs; ++s) {
        if (!(segs[s].flags & 1)) continue;
        uint64_t seg_start = segs[s].address;
        uint64_t seg_end = seg_start + segs[s].size;
        if (seg_end > total_size) seg_end = total_size;
        for (uint64_t p = seg_start; p + 5 < seg_end; ++p) {
            int candidate = (img[p] == 0xF3 && img[p + 1] == 0x0F && img[p + 2] == 0xBD) ||
                            (img[p] == 0xF3 && (img[p + 1] & 0xF0) == 0x40 &&
                             img[p + 2] == 0x0F && img[p + 3] == 0xBD);
            if (!candidate) continue;
            int covered = 0;
            for (uint32_t i = 0; i < s_num_lzcnt_sites; ++i) {
                uintptr_t a = s_lzcnt_sites[i].addr;
                uintptr_t q = (uintptr_t)(img + p);
                if (q >= a && q < a + s_lzcnt_sites[i].len) { covered = 1; break; }
            }
            if (covered) continue;
            if (unconfirmed < 8)
                fprintf(stderr, "Non-BMI1 CPU: LZCNT byte pattern at guest offset 0x%" PRIx64
                                " is not on an instruction boundary; left unpatched\n", p);
            ++unconfirmed;
        }
    }

    if (s_num_lzcnt_sites > 1)
        qsort(s_lzcnt_sites, s_num_lzcnt_sites, sizeof(LzcntPatchSite), compare_lzcnt_sites);

    printf("Non-BMI1 CPU optimizations applied: %u LZCNT UD2 traps installed (exact lengths from Zydis)\n", patched);
    if (skipped)
        fprintf(stderr, "Non-BMI1 CPU: %u LZCNT sites skipped (site table full, increase MAX_LZCNT_SITES)\n", skipped);
    if (unconfirmed)
        fprintf(stderr, "Non-BMI1 CPU: %u raw LZCNT byte patterns ignored (not decoded instructions)\n", unconfirmed);
}

static int emulate_bmi1(CONTEXT *ctx) {
    uintptr_t rip = ctx->Rip;
    if (rip < 0x10000 || IsBadReadPtr((const void *)rip, 8)) return 0;
    const unsigned char *p = (const unsigned char *)rip;

    if (p[0] == 0x0F && p[1] == 0x0B) {
        if (emulate_lzcnt_site(ctx, rip)) return 1;
    }

    if (p[0] != 0xC4) return 0; // 3-byte VEX prefix
    unsigned char b1 = p[1];
    unsigned char b2 = p[2];
    if ((b1 & 0x1F) != 0x02) return 0; // map 2 (0F 38)
    unsigned char opcode = p[3];
    unsigned char modrm = p[4];

    int r_inv = (b1 >> 7) & 1;
    int x_inv = (b1 >> 6) & 1;
    int b_inv = (b1 >> 5) & 1;
    int w = (b2 >> 7) & 1;
    int vvvv = (b2 >> 3) & 0xF;

    int reg = (modrm >> 3) & 7;
    int mod = modrm >> 6;
    int rm = modrm & 7;

    int dest_idx = reg | ((1 - r_inv) << 3);
    int src1_idx = (15 - vvvv);
    int src2_idx = rm | ((1 - b_inv) << 3);

    DWORD64 *regs[16] = {
        &ctx->Rax, &ctx->Rcx, &ctx->Rdx, &ctx->Rbx,
        &ctx->Rsp, &ctx->Rbp, &ctx->Rsi, &ctx->Rdi,
        &ctx->R8,  &ctx->R9,  &ctx->R10, &ctx->R11,
        &ctx->R12, &ctx->R13, &ctx->R14, &ctx->R15
    };

    int insn_len = 5;
    uintptr_t mem_ea = 0;
    if (mod != 3) {
        const unsigned char *cur = p + 5;
        if (rm == 4) {
            unsigned char sib = *cur++;
            insn_len++;
            int scale = 1 << (sib >> 6);
            int index_reg = ((sib >> 3) & 7) | ((1 - x_inv) << 3);
            int sib_base = (sib & 7) | ((1 - b_inv) << 3);
            uint64_t base_val = 0;
            if (mod == 0 && (sib & 7) == 5) {
                int32_t disp = *(const int32_t *)cur;
                cur += 4;
                insn_len += 4;
                base_val = (uint64_t)(int64_t)disp;
            } else {
                base_val = *regs[sib_base];
            }
            uint64_t idx_val = (index_reg != 4) ? *regs[index_reg] : 0;
            mem_ea = base_val + idx_val * scale;
        } else if (mod == 0 && rm == 5) {
            int32_t disp = *(const int32_t *)cur;
            cur += 4;
            insn_len += 4;
            mem_ea = (ctx->Rip + insn_len) + disp;
        } else {
            mem_ea = *regs[src2_idx];
        }
        if (mod == 1) {
            int8_t disp = *(const int8_t *)cur++;
            insn_len += 1;
            mem_ea += disp;
        } else if (mod == 2) {
            int32_t disp = *(const int32_t *)cur;
            cur += 4;
            insn_len += 4;
            mem_ea += disp;
        }
    }

    uint64_t src2_val = 0;
    if (mod == 3) {
        src2_val = *regs[src2_idx];
    } else {
        if (IsBadReadPtr((const void *)mem_ea, w ? 8 : 4)) return 0;
        src2_val = w ? *(const uint64_t *)mem_ea : *(const uint32_t *)mem_ea;
    }

    if (opcode == 0xF2) { // ANDN: dest = (~src1) & src2
        if (w) {
            uint64_t s1 = *regs[src1_idx];
            uint64_t s2 = src2_val;
            uint64_t res = (~s1) & s2;
            *regs[dest_idx] = res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (res == 0) ctx->EFlags |= 0x40;
            if ((int64_t)res < 0) ctx->EFlags |= 0x80;
        } else {
            uint32_t s1 = (uint32_t)*regs[src1_idx];
            uint32_t s2 = (uint32_t)src2_val;
            uint32_t res = (~s1) & s2;
            *regs[dest_idx] = (uint64_t)res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (res == 0) ctx->EFlags |= 0x40;
            if ((int32_t)res < 0) ctx->EFlags |= 0x80;
        }
        ctx->Rip += insn_len;
        return 1;
    } else if (opcode == 0xF3) { // BLSI, BLSMSK, BLSR
        int subop = reg;
        if (w) {
            uint64_t src = src2_val;
            uint64_t res = 0;
            int cf = 0, zf = 0;
            if (subop == 3) { // BLSI
                res = (-src) & src;
                cf = (src != 0);
                zf = (res == 0);
            } else if (subop == 2) { // BLSMSK
                res = (src - 1) ^ src;
                cf = (src == 0);
                zf = 0;
            } else if (subop == 1) { // BLSR
                res = (src - 1) & src;
                cf = (src == 0);
                zf = (res == 0);
            } else {
                return 0;
            }
            *regs[dest_idx] = res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (cf) ctx->EFlags |= 0x01;
            if (zf) ctx->EFlags |= 0x40;
            if ((int64_t)res < 0) ctx->EFlags |= 0x80;
        } else {
            uint32_t src = (uint32_t)src2_val;
            uint32_t res = 0;
            int cf = 0, zf = 0;
            if (subop == 3) { // BLSI
                res = (-src) & src;
                cf = (src != 0);
                zf = (res == 0);
            } else if (subop == 2) { // BLSMSK
                res = (src - 1) ^ src;
                cf = (src == 0);
                zf = 0;
            } else if (subop == 1) { // BLSR
                res = (src - 1) & src;
                cf = (src == 0);
                zf = (res == 0);
            } else {
                return 0;
            }
            *regs[dest_idx] = (uint64_t)res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (cf) ctx->EFlags |= 0x01;
            if (zf) ctx->EFlags |= 0x40;
            if ((int32_t)res < 0) ctx->EFlags |= 0x80;
        }
        ctx->Rip += insn_len;
        return 1;
    } else if (opcode == 0xF7) { // BEXTR: dest = (src2 >> control[7:0]) & ((1 << control[15:8]) - 1)
        uint64_t control = *regs[src1_idx];
        uint32_t start = (uint32_t)(control & 0xFF);
        uint32_t len = (uint32_t)((control >> 8) & 0xFF);
        if (w) {
            uint64_t src = src2_val;
            uint64_t res = 0;
            if (start < 64) {
                src >>= start;
                uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1ULL);
                res = src & mask;
            }
            *regs[dest_idx] = res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (res == 0) ctx->EFlags |= 0x40;
        } else {
            uint32_t src = (uint32_t)src2_val;
            uint32_t res = 0;
            if (start < 32) {
                src >>= start;
                uint32_t mask = (len >= 32) ? ~0U : ((1U << len) - 1U);
                res = src & mask;
            }
            *regs[dest_idx] = (uint64_t)res;
            ctx->EFlags &= ~(0x800 | 0x80 | 0x40 | 0x10 | 0x04 | 0x01);
            if (res == 0) ctx->EFlags |= 0x40;
        }
        ctx->Rip += insn_len;
        return 1;
    }
    return 0;
}
#endif

static void log_redzone_guard_hit(uintptr_t off, uintptr_t rip, uintptr_t rsp, GuestThread *cur) {
    static volatile LONG s_hit_count = 0;
    static ULONGLONG s_last_tick = 0;
    LONG hit = InterlockedIncrement(&s_hit_count);
    ULONGLONG now = GetTickCount64();
    if (hit <= 10 || (now - s_last_tick) >= 1000) {
        s_last_tick = now;
        char buf[2048];
        int pos = snprintf(buf, sizeof(buf),
            "\n[REDZONE GUARD HIT #%ld] Thread '%s' (TID %lu) at guest offset 0x%" PRIxPTR " (RIP %p)\n"
            "  Exceptions previously handled on this thread: %u\n"
            "  RSP: %p\n"
            "  128 bytes below RSP [RSP-128 .. RSP]:\n",
            hit, cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(),
            off, (void *)rip,
            cur ? cur->exception_count : 0,
            (void *)rsp);

        if (rsp >= 128 && rsp < (1ULL << 46) && !IsBadReadPtr((const void *)(rsp - 128), 128)) {
            unsigned char *p = (unsigned char *)(rsp - 128);
            for (int i = 0; i < 128; i += 16) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "    -%02x: ", 128 - i);
                for (int j = 0; j < 16 && pos < (int)sizeof(buf) - 16; j++) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", p[i + j]);
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
            }
            uint64_t val_60 = *(uint64_t *)(rsp - 0x60);
            pos += snprintf(buf + pos, sizeof(buf) - pos, "  Stream pointer at [RSP - 0x60]: 0x%016llx\n",
                           (unsigned long long)val_60);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "    (RSP invalid or unreadable)\n");
        }
        veh_write(buf);
        fprintf(stderr, "%s", buf);
        fflush(stderr);
    }
}

/* the second toggle word (BB_TOGGLE_FILE2, src/runtime_memory.c). Bit 1 is
 * BbToggle::DefensiveGuards in gpu/shim/bbport_toggles.h; this file is C and cannot include
 * that header, so the bit is named here and the two must stay in step. Bit clear: guards on. */
extern uint64_t runtime_disabled_optimizations2;
#define BB_TOGGLE_DEFENSIVE_GUARDS (1ull << 1)

static LONG WINAPI win_veh_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        void *fault_addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t rip = ep->ContextRecord->Rip;
        GuestThread *cur = runtime_thread_current();

        /* The panic printer's abort is a deliberate write to address 0 (0x20b56d2:
         * mov dword ptr [0], 0xdeadba). Classify it before any recovery guard so it cannot be
         * mistaken for a memory bug, and keep the last captured assertion in the dump. */
        if (image && image_size > 0x20b56d2 + 4 && rip == (uintptr_t)image + 0x20b56d2 &&
            !IsBadReadPtr((const void *)rip, 4) &&
            ((const unsigned char *)rip)[0] == 0xc7 &&
            ((const unsigned char *)rip)[1] == 0x04 &&
            ((const unsigned char *)rip)[2] == 0x25) {
            InterlockedExchange(&g_crash_exit_code, 141);
            crash_begin();
            write_crash_dump(ep, fault_addr, rip, code);
            veh_write("[GAME ASSERT] Deliberate abort trap at guest offset 0x20b56d2 "
                      "(the last assertion that reached 0x20b55b0 is in the crash dump)\n");
            if (g_crash_test_hang) Sleep(INFINITE);
            TerminateProcess(GetCurrentProcess(), 141);
        }

        /* fs:-relative access faulted. With FSGSBASE, rebase FS and retry, but only when the
         * base is actually wrong: if FS already equals the guest TCB, rebasing cannot help and
         * a retry would loop forever. Without FSGSBASE the only emulatable form is the 9-byte
         * `mov rax, fs:[0]`; anything else fails loudly instead of retrying. */
        unsigned char *ip = (unsigned char *)rip;
        if (rip >= 0x10000 && !IsBadReadPtr((const void *)rip, 9) && ip[0] == 0x64) {
            void *tcb = runtime_thread_get_tcb();
            int fixed = 0;
            if (tcb && runtime_win_has_fsgsbase() && runtime_win_read_fsbase() != tcb) {
                __asm__ __volatile__("wrfsbase %0" : : "r"(tcb));
                fixed = 1;
            }
            if (tcb && ip[1] == 0x48 && ip[2] == 0x8b && ip[3] == 0x04 && ip[4] == 0x25 &&
                ip[5] == 0x00 && ip[6] == 0x00 && ip[7] == 0x00 && ip[8] == 0x00) {
                ep->ContextRecord->Rax = *(DWORD64 *)tcb;
                ep->ContextRecord->Rip += 9;
                fixed = 1;
            }
            if (fixed) {
                if (cur) cur->exception_count++;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            static volatile LONG fs_guard_warned;
            if (InterlockedExchange(&fs_guard_warned, 1) == 0) {
                char buf[768];
                snprintf(buf, sizeof(buf),
                    "[FS GUARD] Thread '%s' (TID %lu) at guest offset 0x%llx: fs-relative access "
                    "at %p cannot be emulated (CPU lacks FSGSBASE and this is not the 9-byte "
                    "mov rax, fs:[0] form). This CPU is not fully supported; failing instead of "
                    "retrying the fault.\n",
                    cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(),
                    image && is_guest_address(rip) ? (unsigned long long)(rip - (uintptr_t)image) : 0,
                    fault_addr);
                veh_write(buf);
                fprintf(stderr, "%s", buf);
                fflush(stderr);
            }
            /* fall through to the fatal path (no infinite retry) */
        }
        if (gpu_enabled && bbgpu_handle_fault(ep, fault_addr)) {
            if (cur) cur->exception_count++;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Camera update thunk DEP or unmapped jump safeguard (Discord crash 204614)
        ULONG_PTR access_type = (ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 1) ?
                                ep->ExceptionRecord->ExceptionInformation[0] : 0;
        if (access_type == 8 /* EXECUTE violation */ || rip < 0x10000 || rip == (uintptr_t)orig_cam_update) {
            uintptr_t expected_orig = (uintptr_t)image + 0x143ac60;
            uintptr_t rsp = ep->ContextRecord->Rsp;
            if (rsp >= 0x10000 && !IsBadReadPtr((const void *)rsp, 8)) {
                uintptr_t ret_candidate = *(DWORD64 *)rsp;
                if (is_guest_address(ret_candidate)) {
                    uintptr_t ret_off = ret_candidate - (uintptr_t)image;
                    if (ret_off == 0x1436c05 || ret_off == 0x1436e1a) {
                        char buf[512];
                        snprintf(buf, sizeof(buf),
                            "[NATIVE MOUSE THUNK GUARD] Thread '%s' (TID %lu): "
                            "Intercepted execution violation at %p. Redirecting to original camera update at %p.\n",
                            cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(), (void *)rip, (void *)expected_orig);
                        veh_write(buf);
                        fprintf(stderr, "%s", buf);
                        fflush(stderr);

                        orig_cam_update = (void (ABI *)(void*, void*, void*))expected_orig;
                        ep->ContextRecord->Rip = expected_orig;
                        if (cur) cur->exception_count++;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
        }
        if (is_guest_address(rip)) {
            uintptr_t off = rip - (uintptr_t)image;

            // Issue #140 / Discord 231500: Graphics worker job dispatch NULL or culled batch
            if (off >= 0x21b4b30 && off <= 0x21b4b55) {
                uintptr_t rbp = ep->ContextRecord->Rbp;
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "[GRAPHICS WORKER GUARD] Thread '%s' (TID %lu) at guest offset 0x%" PRIxPTR ": "
                    "Invalid graphics job batch (fault_addr=%p, RAX=%p, R14=%p, R15=%p). Unwinding to caller 0x%" PRIx64 ".\n",
                    cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(), off,
                    fault_addr, (void *)ep->ContextRecord->Rax, (void *)ep->ContextRecord->R14,
                    (void *)ep->ContextRecord->R15, *(DWORD64 *)(rbp + 0x08) - (uintptr_t)image);
                veh_write(buf);
                fprintf(stderr, "%s", buf);
                fflush(stderr);

                ep->ContextRecord->Rbx = *(DWORD64 *)(rbp - 0x20);
                ep->ContextRecord->R12 = *(DWORD64 *)(rbp - 0x18);
                ep->ContextRecord->R14 = *(DWORD64 *)(rbp - 0x10);
                ep->ContextRecord->R15 = *(DWORD64 *)(rbp - 0x08);
                ep->ContextRecord->Rip = *(DWORD64 *)(rbp + 0x08);
                ep->ContextRecord->Rsp = rbp + 0x10;
                ep->ContextRecord->Rbp = *(DWORD64 *)(rbp);
                ep->ContextRecord->Rax = 0;
                if (cur) cur->exception_count++;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // DLTemporaryHeap NULL initialization recovery with real 128KB arena
            // Gated by runtime verification of CUSA03173 v1.09
            if ((off >= 0x207de20 && off <= 0x207de2a) && ep->ContextRecord->Rcx == 0 && is_cusa03173_v109()) {
                static __thread uint8_t s_thread_temp_heap[131072] __attribute__((aligned(64)));
                uint8_t *heap = s_thread_temp_heap;
                uint64_t *hdr = (uint64_t *)heap;
                uint64_t start = (uint64_t)(uintptr_t)(heap + 0x38);
                uint64_t end = (uint64_t)(uintptr_t)(heap + sizeof(s_thread_temp_heap));
                hdr[0] = start;
                hdr[1] = end;
                hdr[2] = start;
                hdr[3] = end;
                hdr[4] = (uint64_t)(uintptr_t)image + 0x535b540;
                hdr[5] = (uint64_t)(uintptr_t)image + 0x53b3150;
                hdr[6] = (uint64_t)(uintptr_t)(heap + 0x20);
                void *tcb = runtime_thread_get_tcb();
                if (tcb) {
                    *(uint64_t *)((char *)tcb - 1840) = (uint64_t)(uintptr_t)heap;
                }
                ep->ContextRecord->Rcx = (DWORD64)(uintptr_t)heap;
                if (cur) cur->exception_count++;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Issue #136: Bucket allocator refill loop NULL pointer write
            if (off == 0x443025 && ep->ContextRecord->Rdi == 0) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    "[ALLOCATOR GUARD] Thread '%s' (TID %lu) at guest offset 0x443025: "
                    "NULL bucket chunk pointer in refill loop (ECX=%u remaining). Breaking chunk link loop.\n",
                    cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(),
                    (unsigned int)ep->ContextRecord->Rcx);
                veh_write(buf);
                fprintf(stderr, "%s", buf);
                fflush(stderr);

                ep->ContextRecord->Rip = (uintptr_t)image + 0x443037;
                if (cur) cur->exception_count++;
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // allocate valid destination buffer R13 and replay copy to eliminate cascade
            // Gated by runtime verification of CUSA03173 v1.09
            if (off >= 0x21d5b45 && off < 0x21d5d71 && ep->ContextRecord->R13 == 0 && is_cusa03173_v109()) {
                void *dst = runtime_low_alloc_aligned(2048, 64);
                if (dst) {
                    uintptr_t src = ep->ContextRecord->Rbx;
                    if (src && !IsBadReadPtr((const void *)src, 0x398)) {
                        memcpy(dst, (const void *)src, 0x398);
                    }
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "[ASSET COPY GUARD] Thread '%s' (TID %lu) at guest offset 0x%" PRIxPTR ": "
                        "NULL destination R13 restored with low-alloc buffer %p. Resuming structure copy.\n",
                        cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(), off, dst);
                    veh_write(buf);
                    fprintf(stderr, "%s", buf);
                    fflush(stderr);

                    ep->ContextRecord->R13 = (DWORD64)(uintptr_t)dst;
                    ep->ContextRecord->Rip = (uintptr_t)image + 0x21d5b45;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // 0x2199310 stores r13 through r12, the block the allocator called at
            // 0x21992fb (size edx=0x10) returned; r12 == 0 when that allocation failed
            // (log: "Guest fault (signal 11) at guest offset 0x2199310, address ...0008").
            // Same shape as the 0x21d5b45 guard above: supply a real block and replay the store.
            if (off == 0x2199310 && ep->ContextRecord->R12 == 0 && is_cusa03173_v109() &&
                (__atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED) & BB_TOGGLE_DEFENSIVE_GUARDS) == 0) {
                void *block = runtime_low_alloc_aligned(16, 16);
                if (block) {
                    static volatile LONG warned;
                    if (InterlockedExchange(&warned, 1) == 0) {
                        const uintptr_t rsp = ep->ContextRecord->Rsp;
                        uintptr_t ret = 0;
                        if (rsp && !IsBadReadPtr((const void *)rsp, sizeof(ret)))
                            ret = *(const uintptr_t *)rsp;
                        char ret_buf[96];
                        if (ret && is_guest_address(ret))
                            snprintf(ret_buf, sizeof(ret_buf), "0x%" PRIxPTR " (guest 0x%" PRIxPTR ")",
                                     ret, ret - (uintptr_t)image);
                        else
                            snprintf(ret_buf, sizeof(ret_buf), "%p", (void *)ret);
                        char buf[768];
                        snprintf(buf, sizeof(buf),
                            "[NULL STORE GUARD] Thread '%s' (TID %lu) at guest offset 0x%" PRIxPTR ": "
                            "r12 (16-byte allocation) was NULL; r13=%p stored into restored block %p, "
                            "return %s.\n",
                            cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(), off,
                            (void *)(uintptr_t)ep->ContextRecord->R13, block, ret_buf);
                        veh_write(buf);
                        fprintf(stderr, "%s", buf);
                        fflush(stderr);
                    }
                    ep->ContextRecord->R12 = (DWORD64)(uintptr_t)block;
                    ep->ContextRecord->Rip = (uintptr_t)image + 0x2199310;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // 0x4370b6 initialises the block [vtable+0x10](this, 0x28) returned;
            // rax == 0 when that allocation failed (log: "Guest fault (signal 11) at guest
            // offset 0x4370b6, address 0000000000000000 [NULL]"). 0x4370bd..0x4370e1 then
            // initialise those 0x28 bytes in place, so a zeroed block is the expected state.
            if (off == 0x4370b6 && ep->ContextRecord->Rax == 0 && is_cusa03173_v109() &&
                (__atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED) & BB_TOGGLE_DEFENSIVE_GUARDS) == 0) {
                void *block = runtime_low_alloc_aligned(0x28, 16);
                if (block) {
                    memset(block, 0, 0x28);
                    static volatile LONG warned;
                    if (InterlockedExchange(&warned, 1) == 0) {
                        const uint64_t this_ptr = ep->ContextRecord->R12;
                        uint64_t vtable = 0;
                        if (this_ptr && !IsBadReadPtr((const void *)(uintptr_t)this_ptr, sizeof(vtable)))
                            vtable = *(const uint64_t *)(uintptr_t)this_ptr;
                        char buf[768];
                        snprintf(buf, sizeof(buf),
                            "[NULL ALLOC GUARD] Thread '%s' (TID %lu) at guest offset 0x%" PRIxPTR ": "
                            "40-byte allocation returned NULL; vtable [r12]=%p, this r12=%p rcx=%p, "
                            "size rsi=0x%llx, restored block %p.\n",
                            cur ? cur->name : "unknown", (unsigned long)GetCurrentThreadId(), off,
                            (void *)(uintptr_t)vtable, (void *)(uintptr_t)this_ptr,
                            (void *)(uintptr_t)ep->ContextRecord->Rcx,
                            (unsigned long long)ep->ContextRecord->Rsi, block);
                        veh_write(buf);
                        fprintf(stderr, "%s", buf);
                        fflush(stderr);
                    }
                    ep->ContextRecord->Rax = (DWORD64)(uintptr_t)block;
                    ep->ContextRecord->Rip = (uintptr_t)image + 0x4370b6;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Issue #127: particle, blood splatter, and cloth vertex stream generators
            if (off >= 0x28ce7ba && off < 0x28d064e) {
                uintptr_t rsp = ep->ContextRecord->Rsp;
                log_redzone_guard_hit(off, rip, rsp, cur);
                if (off >= 0x28ce7ba && off < 0x28ceeb8) { // 0x28ce7b0 (sub $0x30, %rsp)
                    ep->ContextRecord->Rbx = *(DWORD64 *)(rsp + 0x30);
                    ep->ContextRecord->R12 = *(DWORD64 *)(rsp + 0x38);
                    ep->ContextRecord->R13 = *(DWORD64 *)(rsp + 0x40);
                    ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x48);
                    ep->ContextRecord->R15 = *(DWORD64 *)(rsp + 0x50);
                    ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x58);
                    ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x60);
                    ep->ContextRecord->Rsp = rsp + 0x68;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else if (off >= 0x28ceeca && off < 0x28cf572) { // 0x28ceec0 (sub $0x20, %rsp)
                    ep->ContextRecord->Rbx = *(DWORD64 *)(rsp + 0x20);
                    ep->ContextRecord->R12 = *(DWORD64 *)(rsp + 0x28);
                    ep->ContextRecord->R13 = *(DWORD64 *)(rsp + 0x30);
                    ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x38);
                    ep->ContextRecord->R15 = *(DWORD64 *)(rsp + 0x40);
                    ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x48);
                    ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x50);
                    ep->ContextRecord->Rsp = rsp + 0x58;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else if (off >= 0x28cf58a && off < 0x28cfb95) { // 0x28cf580 (push %rax = 8 bytes)
                    ep->ContextRecord->Rbx = *(DWORD64 *)(rsp + 0x08);
                    ep->ContextRecord->R12 = *(DWORD64 *)(rsp + 0x10);
                    ep->ContextRecord->R13 = *(DWORD64 *)(rsp + 0x18);
                    ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x20);
                    ep->ContextRecord->R15 = *(DWORD64 *)(rsp + 0x28);
                    ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x30);
                    ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x38);
                    ep->ContextRecord->Rsp = rsp + 0x40;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else if (off >= 0x28cfbaa && off < 0x28d015e) { // 0x28cfba0 (sub $0x18, %rsp)
                    ep->ContextRecord->Rbx = *(DWORD64 *)(rsp + 0x18);
                    ep->ContextRecord->R12 = *(DWORD64 *)(rsp + 0x20);
                    ep->ContextRecord->R13 = *(DWORD64 *)(rsp + 0x28);
                    ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x30);
                    ep->ContextRecord->R15 = *(DWORD64 *)(rsp + 0x38);
                    ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x40);
                    ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x48);
                    ep->ContextRecord->Rsp = rsp + 0x50;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else if ((off >= 0x28d0164 && off < 0x28d02b4) ||
                           (off >= 0x28d02c4 && off < 0x28d046b) ||
                           (off >= 0x28d0484 && off < 0x28d0505)) { // 0x28d0160, 0x28d02c0, 0x28d0480 (push rbp, r14, rbx)
                    ep->ContextRecord->Rbx = *(DWORD64 *)(rsp);
                    ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x08);
                    ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x10);
                    ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x18);
                    ep->ContextRecord->Rsp = rsp + 0x20;
                    if (cur) cur->exception_count++;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        if (runtime_fault_recover) {
            RuntimeRecoverBuf *recover = runtime_fault_recover;
            runtime_fault_recover = NULL;
            RUNTIME_RECOVER_JUMP(*recover);
        }
        /* Host window hooks (e.g. Fasoo DRM) use IsBadReadPtr during window
         * creation. Its first-chance AV belongs to Windows' own SEH handler.
         * Keep GPU page tracking and speculative guest recovery ahead of it. */
        if (win_fault_is_read_probe(rip)) return EXCEPTION_CONTINUE_SEARCH;
        const int guest_code = is_guest_address(rip);
        /* A guest NULL dereference is the classic symptom of an allocation that returned 0
         * (issues #148/#151/#155). A distinct exit code lets the launcher tell it apart from a
         * random host fault, and the dump now records allocation failures and the last assert. */
        const int guest_null = guest_code && (uintptr_t)fault_addr < 0x10000;
        InterlockedExchange(&g_crash_exit_code, guest_null ? 140 : 128 + 11);
        crash_begin();
        write_crash_dump(ep, fault_addr, rip, code);
        char fault_loc[256];
        describe_pointer((uintptr_t)fault_addr, fault_loc, sizeof(fault_loc));
        char line[768];
        if (guest_code)
            snprintf(line, sizeof(line),
                     "Guest fault (signal 11) at guest offset 0x%llx, address %p [%s]\n",
                     (unsigned long long)(rip - (uintptr_t)image), fault_addr, fault_loc);
        else
            snprintf(line, sizeof(line), "Fault at RIP %p, address %p [%s]\n",
                     (void *)rip, fault_addr, fault_loc);
        veh_write(line);
        snprintf(line, sizeof(line), "RAX=%p RBX=%p RCX=%p RDX=%p RSI=%p RDI=%p RBP=%p RSP=%p\n",
                 (void *)ep->ContextRecord->Rax, (void *)ep->ContextRecord->Rbx,
                 (void *)ep->ContextRecord->Rcx, (void *)ep->ContextRecord->Rdx,
                 (void *)ep->ContextRecord->Rsi, (void *)ep->ContextRecord->Rdi,
                 (void *)ep->ContextRecord->Rbp, (void *)ep->ContextRecord->Rsp);
        veh_write(line);
        void *cur_fs = runtime_win_read_fsbase();
        snprintf(line, sizeof(line), "FS_BASE=%p GS_SELECTOR=0x%x\n", cur_fs, (unsigned int)ep->ContextRecord->SegGs);
        veh_write(line);
        if (gpu_enabled) bbgpu_dump_guest_writes(ep);
        if (g_crash_test_hang) Sleep(INFINITE);
        TerminateProcess(GetCurrentProcess(), guest_null ? 140 : 128 + 11);
    } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        if (!runtime_win_has_bmi1() && emulate_bmi1(ep->ContextRecord)) {
            GuestThread *cur = runtime_thread_current();
            if (cur) cur->exception_count++;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        uintptr_t rip = ep->ContextRecord->Rip;
        InterlockedExchange(&g_crash_exit_code, 128 + 4);
        crash_begin();
        write_crash_dump(ep, (void *)rip, rip, code);
        char line[512];
        if (is_guest_address(rip))
            snprintf(line, sizeof(line), "Fatal exception 0x%08lx at guest offset 0x%llx, address %p\n",
                     (unsigned long)code, (unsigned long long)(rip - (uintptr_t)image), (void *)rip);
        else
            snprintf(line, sizeof(line), "Fatal exception 0x%08lx at RIP %p, address %p\n", (unsigned long)code, (void *)rip, (void *)rip);
        veh_write(line);
        TerminateProcess(GetCurrentProcess(), 128 + 4);
    } else if (code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
               code == EXCEPTION_STACK_OVERFLOW ||
               code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */ ||
               code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */) {
        uintptr_t rip = ep->ContextRecord->Rip;
        InterlockedExchange(&g_crash_exit_code, 128 + 4);
        crash_begin();
        write_crash_dump(ep, (void *)rip, rip, code);
        char line[512];
        if (is_guest_address(rip))
            snprintf(line, sizeof(line), "Fatal exception 0x%08lx at guest offset 0x%llx, address %p\n",
                     (unsigned long)code, (unsigned long long)(rip - (uintptr_t)image), (void *)rip);
        else
            snprintf(line, sizeof(line), "Fatal exception 0x%08lx at RIP %p, address %p\n", (unsigned long)code, (void *)rip, (void *)rip);
        veh_write(line);
        TerminateProcess(GetCurrentProcess(), 128 + 4);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) {
    puts("Runtime: process finalizer callback reached");
    print_exit_summary();
}
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
#ifndef _WIN32
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. Windows has no exec: the old process starts
 * run.bat and terminates itself. Not with ExitProcess, whose DLL detach (GPU driver, audio)
 * hung after every restart and kept the old process alive with its RAM and VRAM. */
void runtime_restart(void) {
    fflush(NULL);
#ifndef _WIN32
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#else
    puts("Runtime: restarting through run.bat");
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[] = "cmd.exe /c run.bat";
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        print_exit_summary();
        fflush(NULL);
        TerminateProcess(GetCurrentProcess(), 0);
    } else {
        fail("runtime_restart: CreateProcess failed");
    }
#endif
}

/* The measured-dt base patch (60 FPS++ (Deltatime)) adds a guest-global simulation dt at
 * image+0x535A908. Its injected block writes it at 0x2018e3d, which our hook replaces, so the
 * hook mirrors the store here. Only enabled when the base patch's code cave is present. */
static int g_live_clock_deltatime;
static float s_smoothed_dt;
static int s_prev_target_fps = -1;

/* Live engine clock (Phase 2). Called from the 25-byte guest stub installed at 0x2018e3d with
 * rdi = clock singleton (rbx). No allocation, no locks: relaxed atomic reads from BbSettings
 * and two stores into the guest singleton. Returns the simulation dt in xmm0; the stub writes
 * it to the frame payload at [rbp-0x38] and resumes at 0x2018e6b. */
ABI float bb_live_clock_tick(void *singleton) {
    if (!singleton) return 0.0f;
    float *interval = (float *)((char *)singleton + 0x18);
    int mode = 0, target_fps = 60, max_dt_hz = 30;
    bbgpu_get_live_clock(&mode, &target_fps, &max_dt_hz);
    if (mode == 0) return *interval; /* static XML patches: no behaviour change */
    const float measured = *(float *)((char *)singleton + 0x264);
    const float target = target_fps > 0 ? 1.0f / (float)target_fps : 1.0f / 240.0f;
    float max_dt = max_dt_hz > 0 ? 1.0f / (float)max_dt_hz : (1.0f / 30.0f);
    if (max_dt < target) max_dt = target;
    *interval = target;

    float raw = measured > 0.0f ? measured : target;
    if (raw > max_dt) raw = max_dt;
    if (raw < 1.0f / 240.0f) raw = 1.0f / 240.0f;

    /* Deadband: if measured dt is within ±8% of target frametime, lock to exact target.
     * Wipes out OS timer/vsync micro-jitter so foliage and Havok cloth stay rock-steady at target FPS. */
    if (target_fps > 0) {
        const float deadband = target * 0.08f;
        if (raw >= (target - deadband) && raw <= (target + deadband)) {
            raw = target;
        }
    }

    /* Seed immediately on initial tick or smoothly slew when target FPS is switched in the menu */
    if (s_smoothed_dt <= 0.0f) {
        s_smoothed_dt = target;
        s_prev_target_fps = target_fps;
    } else {
        if (s_prev_target_fps != target_fps) {
            s_prev_target_fps = target_fps;
            /* Prevent UI menu dropdown stalls during target switch from injecting spikes */
            if (target_fps > 0 && raw > target && raw > s_smoothed_dt) {
                raw = target > s_smoothed_dt ? target : s_smoothed_dt;
            }
        }

        /* Slew-rate limiter / spike dampener: prevent sudden impulse shocks to Havok cloth
         * constraints and foliage accumulators (+12% max increase, -15% max decrease per frame). */
        const float max_increase = s_smoothed_dt * 0.12f;
        const float max_decrease = s_smoothed_dt * 0.15f;
        if (raw > s_smoothed_dt) {
            float step = raw - s_smoothed_dt;
            if (step > max_increase) step = max_increase;
            s_smoothed_dt += step;
        } else if (raw < s_smoothed_dt) {
            float step = s_smoothed_dt - raw;
            if (step > max_decrease) step = max_decrease;
            s_smoothed_dt -= step;
        }
    }

    float dt = s_smoothed_dt;
    if (dt > max_dt) dt = max_dt;
    if (dt < 1.0f / 240.0f) dt = 1.0f / 240.0f;

    /* Deliver the filtered simulation dt directly to the clock singleton's measured dt slot.
     * All 31 in-engine code caves (Havok cloth, character physics, grass/flower foliage wind
     * accumulators) sample [singleton + 0x264]. Overwriting the raw OS timer reading with our
     * deadbanded and smoothed dt eliminates high-frequency wind flutter and cloth vibration. */
    *(float *)((char *)singleton + 0x264) = dt;

    if (g_live_clock_deltatime && image) {
        *(float *)(image + 0x535A908) = dt;
    }
    bbgpu_note_live_clock(dt, target_fps);
    return dt;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    runtime_memory_reserve_space();
    int avx_status = runtime_win_check_avx();
    if (avx_status != 1) {
        if (avx_status == 0) {
            fprintf(stderr, "\n================================================================================\n"
                            "ERROR: Host CPU lacks AVX (Advanced Vector Extensions) instruction support!\n"
                            "Bloodborne and PS4 system libraries are compiled with -mavx and strictly require\n"
                            "AVX 1.0 support. Your processor (e.g. pre-Sandy Bridge / Nehalem) cannot run this title.\n"
                            "================================================================================\n\n");
            MessageBoxA(NULL,
                        "Your processor does not support AVX (Advanced Vector Extensions 1.0).\n\n"
                        "Bloodborne and PS4 system binaries natively require AVX support. Pre-Sandy Bridge\n"
                        "CPUs (such as Intel Core 2 Quad or Nehalem/Westmere Xeon X56xx) cannot execute these instructions.\n\n"
                        "The application cannot start.",
                        "Bloodborne PC - Unsupported CPU (No AVX)", MB_OK | MB_ICONERROR);
        } else {
            fprintf(stderr, "\n================================================================================\n"
                            "ERROR: AVX instructions are supported by CPU but disabled by OS / Hypervisor (OSXSAVE/XCR0)!\n"
                            "Please ensure Windows XSAVE / YMM state support and virtualization settings allow AVX.\n"
                            "================================================================================\n\n");
            MessageBoxA(NULL,
                        "Your processor supports AVX, but the operating system or hypervisor has disabled\n"
                        "AVX state saving (OSXSAVE / XCR0 YMM state).\n\n"
                        "If you are running in a virtual machine or hypervisor, enable AVX/XSAVE passthrough.\n\n"
                        "The application cannot start.",
                        "Bloodborne PC - AVX Disabled by OS", MB_OK | MB_ICONERROR);
        }
        return 132;
    }
#endif
    time_t t_now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t_now);
#else
    localtime_r(&t_now, &tm_buf);
#endif
    strftime(g_run_timestamp, sizeof(g_run_timestamp), "%Y%m%d_%H%M%S", &tm_buf);

    const char *env_trace = getenv("BB_TRACE");
    if (env_trace && (!strcmp(env_trace, "1") || !strcmp(env_trace, "true") || !strcmp(env_trace, "yes"))) {
        g_bb_trace = 1;
    }

    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--trace")) g_bb_trace = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }

    if (g_bb_trace) {
        char imp_ts_path[128];
        snprintf(imp_ts_path, sizeof(imp_ts_path), "out/imports_%s.log", g_run_timestamp);
        g_import_file = fopen(imp_ts_path, "w");
        if (!g_import_file) g_import_file = fopen("out/imports.log", "w");
        if (!g_import_file) g_import_file = fopen("imports.log", "w");
        if (g_import_file) setvbuf(g_import_file, NULL, _IOFBF, 64 * 1024);
        printf("[BB_TRACE] Tracing enabled: per-second heartbeat, hang snapshots, and import logging active.\n");
    } else {
        printf("[FAST PATH] Tracing disabled (BB_TRACE=0): direct import dispatch active.\n");
    }
    atexit(print_exit_summary);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    timeBeginPeriod(1);
    if (!runtime_win_has_fsgsbase()) {
        fprintf(stderr,
            "Warning: host CPU lacks FSGSBASE (wrfsbase/rdfsbase). This CPU is NOT fully\n"
            "         supported: only the single 9-byte `mov rax, fs:[0]` initial-TLS access can\n"
            "         be emulated. Any other fs:-relative guest access stops at the [FS GUARD]\n"
            "         diagnostic instead of retrying forever. Use a Haswell/Excavator-or-newer CPU.\n");
    }
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, win_veh_handler);
    start_crash_reaper();
    start_timeout(timeout_seconds);
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    start_timeout(timeout_seconds);
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE_ID",s_game_title_id,sizeof(s_game_title_id),NULL);
        sfo_value(sfo,"APP_VER",s_game_app_ver,sizeof(s_game_app_ver),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    image_size = size;
    resolved_targets = calloc(import_count + 1, sizeof(void *));
    import_call_counts = calloc(import_count + 1, sizeof(uint64_t));
    is_unbound = calloc(import_count + 1, sizeof(uint8_t));
    if (g_bb_trace) {
        mo_call_counts = calloc(import_count + 1, sizeof(uint64_t));
    }
    for (uint64_t i = 0; i < import_count; ++i) {
        uintptr_t resolved = runtime_resolve(names[i], 0);
        if (resolved) {
            resolved_targets[i] = (void *)resolved;
            is_unbound[i] = 0;
        } else if (native_libc && bindings[i]) {
            resolved_targets[i] = (void *)(image + bindings[i]);
            is_unbound[i] = 0;
        } else {
            resolved_targets[i] = NULL;
            is_unbound[i] = 1;
        }
    }
    unsigned char *thunks = allocate(round_page((import_count + 16) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = thunks + i * 32;
        uint32_t index = (uint32_t)i;
        uintptr_t dispatch = (uintptr_t)common_dispatch;
        t[0] = 0x41; t[1] = 0xbb; memcpy(t + 2, &index, 4);          /* mov $index, %r11d */
        t[6] = 0x48; t[7] = 0xb8; memcpy(t + 8, &dispatch, 8);       /* movabs $dispatch, %rax */
        t[16] = 0xff; t[17] = 0xe0;                                   /* jmp *%rax */
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = 0;
        if (relocs[i].kind == 2) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], 1);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                value = (uintptr_t)image + bindings[relocs[i].value] + relocs[i].addend;
            } else {
                value = (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend);
            }
        } else if (relocs[i].kind == 1) {
            uint64_t imp_idx = relocs[i].value;
            uintptr_t resolved = runtime_resolve(names[imp_idx], 0);
            if (resolved) {
                if (g_bb_trace) {
                    value = (uintptr_t)(thunks + 32 * imp_idx);
                } else {
                    value = resolved + relocs[i].addend;
                }
            } else if (native_libc && bindings[imp_idx]) {
                uint64_t address = bindings[imp_idx];
                if (binding_kinds[imp_idx] != relocs[i].kind || !mapped(segments, ns, address, relocs[i].addend + 1))
                    fail("native export kind/range mismatch");
                value = (uintptr_t)image + address + relocs[i].addend;
            } else {
                value = (uintptr_t)(thunks + 32 * imp_idx);
            }
        } else {
            value = (uintptr_t)image + relocs[i].value;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
#ifdef _WIN32
    if (!runtime_win_has_bmi1()) {
        for (uint64_t i = 0; i < import_count; ++i) {
            if (!strcmp(names[i], "j4ViWNHEgww#q#q") && bindings[i]) {
                uint64_t addr = bindings[i];
                // Scalar strlen replacement:
                // 48 89 f8             mov %rdi, %rax
                // 80 38 00             1: cmpb $0, (%rax)
                // 74 05                je 2f
                // 48 ff c0             inc %rax
                // eb f6                jmp 1b
                // 48 29 f8             2: sub %rdi, %rax
                // c3                   ret
                static const unsigned char scalar_strlen[17] = {
                    0x48, 0x89, 0xf8, 0x80, 0x38, 0x00, 0x74, 0x05,
                    0x48, 0xff, 0xc0, 0xeb, 0xf6, 0x48, 0x29, 0xf8, 0xc3
                };
                if (addr + sizeof(scalar_strlen) <= size) {
                    memcpy(image + addr, scalar_strlen, sizeof(scalar_strlen));
                    printf("Applied scalar strlen patch at guest offset 0x%" PRIx64 " for non-BMI1 CPU\n", addr);
                }
                break;
            }
        }
        patch_non_bmi1(image, size, segments, ns);
    }
#endif
    /* Install native mouse camera hook only when enabled in settings AND binary signatures match */
    if (!bbgpu_get_mk_native_mouse()) {
        printf("[NATIVE MOUSE] Native mouse look disabled in settings; call sites remain original, stick emulation active.\n");
    } else {
        do {
            if (size <= 0x183ac60) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] image size 0x%" PRIx64 " too small for camera offsets; skipping hook.\n", size);
                break;
            }

            static const unsigned char site1_expected[5] = { 0xe8, 0x5b, 0x40, 0x00, 0x00 };
            static const unsigned char site2_expected[5] = { 0xe8, 0x46, 0x3e, 0x00, 0x00 };
            static const unsigned char target_expected[4] = { 0x55, 0x48, 0x89, 0xe5 };

            if (memcmp(image + 0x1436c00, site1_expected, 5) != 0) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Site 1 at image+0x1436c00 signature mismatch; skipping hook.\n");
                break;
            }
            if (memcmp(image + 0x1436e15, site2_expected, 5) != 0) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Site 2 at image+0x1436e15 signature mismatch; skipping hook.\n");
                break;
            }
            if (memcmp(image + 0x143ac60, target_expected, 4) != 0) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Target func at image+0x143ac60 signature mismatch; skipping hook.\n");
                break;
            }

            unsigned char *t = thunks + import_count * 32;
            uintptr_t hook_fn = (uintptr_t)guest_camera_update_thunk;
            /* 14-byte rip-relative jmp without clobbering rax or any other register: jmp *(%rip + 0) */
            t[0] = 0xff; t[1] = 0x25; t[2] = 0x00; t[3] = 0x00; t[4] = 0x00; t[5] = 0x00;
            memcpy(t + 6, &hook_fn, 8);

            int64_t disp1 = (int64_t)t - (int64_t)(image + 0x1436c00 + 5);
            int64_t disp2 = (int64_t)t - (int64_t)(image + 0x1436e15 + 5);
            if (disp1 < INT32_MIN || disp1 > INT32_MAX || disp2 < INT32_MIN || disp2 > INT32_MAX) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Hook displacement out of 32-bit range (d1=%" PRId64 ", d2=%" PRId64 "); skipping hook.\n", disp1, disp2);
                break;
            }

            orig_cam_update = (void (ABI *)(void*, void*, void*))(image + 0x143ac60);

            int32_t d1 = (int32_t)disp1;
            int32_t d2 = (int32_t)disp2;
            memcpy(image + 0x1436c01, &d1, 4);
            memcpy(image + 0x1436e16, &d2, 4);

            g_native_camera_hook_installed = 1;
            printf("[NATIVE MOUSE] Direct camera rotation hook installed successfully at image+0x1436c00 and image+0x1436e15 (thunk=%p, orig=%p)\n", (void*)t, (void*)orig_cam_update);
        } while (0);

        /* Aim camera (monocular) update, called at image+0x1436d2f and image+0x1436d5e */
        do {
            if (!g_native_camera_hook_installed) break;
            static const unsigned char aim1_expected[5] = { 0xe8, 0x2c, 0xcb, 0xff, 0xff };
            static const unsigned char aim2_expected[5] = { 0xe8, 0xfd, 0xca, 0xff, 0xff };
            static const unsigned char aim_target_expected[4] = { 0x55, 0x48, 0x89, 0xe5 };
            if (memcmp(image + 0x1436d2f, aim1_expected, 5) != 0 ||
                memcmp(image + 0x1436d5e, aim2_expected, 5) != 0 ||
                memcmp(image + 0x1433860, aim_target_expected, 4) != 0) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Aim camera signature mismatch; monocular mouse look not hooked.\n");
                break;
            }

            /* Slots import_count+2..+7 belong to the assert hook stub. */
            unsigned char *t = thunks + (import_count + 8) * 32;
            uintptr_t hook_fn = (uintptr_t)guest_aim_camera_update_thunk;
            t[0] = 0xff; t[1] = 0x25; t[2] = 0x00; t[3] = 0x00; t[4] = 0x00; t[5] = 0x00;
            memcpy(t + 6, &hook_fn, 8);

            int64_t disp1 = (int64_t)t - (int64_t)(image + 0x1436d2f + 5);
            int64_t disp2 = (int64_t)t - (int64_t)(image + 0x1436d5e + 5);
            if (disp1 < INT32_MIN || disp1 > INT32_MAX || disp2 < INT32_MIN || disp2 > INT32_MAX) {
                fprintf(stderr, "[NATIVE MOUSE GUARD] Aim camera hook displacement out of 32-bit range; skipping hook.\n");
                break;
            }

            orig_aim_cam_update = (void (ABI *)(void*, void*))(image + 0x1433860);
            int32_t d1 = (int32_t)disp1;
            int32_t d2 = (int32_t)disp2;
            memcpy(image + 0x1436d30, &d1, 4);
            memcpy(image + 0x1436d5f, &d2, 4);
            printf("[NATIVE MOUSE] Aim camera (monocular) hook installed at image+0x1436d2f and image+0x1436d5e\n");
        } while (0);
    }
    if (!cpu_only) bbgpu_menu_pointer_install(image, size);

    /* Blur-kernel sample count clamp (0xa0020007 / crash 23 guard at GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0) */
    do {
        const char *clamp_env = getenv("BB_KERNEL_CLAMP");
        if (clamp_env && clamp_env[0] == '0') break;
        if (size <= 0xbbc3e0 + 6) break;
        static const unsigned char expected_entry[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
        if (memcmp(image + 0xbbc3e0, expected_entry, sizeof(expected_entry)) != 0) {
            printf("Guest hooks: blur-kernel sample count clamp not installed (unmatched signature)\n");
            break;
        }
        unsigned char *t = thunks + (import_count + 1) * 32;
        /* Thunk implementation:
         * 83 ff 03                cmp    $0x3, %edi
         * 7d 05                   jge    over
         * bf 03 00 00 00          mov    $0x3, %edi
         * 55 48 89 e5 41 57       (over: entry instructions)
         * ff 25 00 00 00 00       jmp    *(%rip)
         * <8-byte return address> (image + 0xbbc3e0 + 6)
         */
        unsigned char stub[30] = {
            0x83, 0xff, 0x03,
            0x7d, 0x05,
            0xbf, 0x03, 0x00, 0x00, 0x00,
            0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
            0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
            0, 0, 0, 0, 0, 0, 0, 0
        };
        uint64_t return_addr = (uint64_t)(image + 0xbbc3e0 + 6);
        memcpy(stub + 22, &return_addr, 8);
        memcpy(t, stub, sizeof(stub));

        int64_t disp = (int64_t)t - (int64_t)(image + 0xbbc3e0 + 5);
        if (disp < INT32_MIN || disp > INT32_MAX) {
            printf("Guest hooks: blur-kernel clamp thunk displacement out of 32-bit range\n");
            break;
        }
        int32_t rel = (int32_t)disp;
        unsigned char jmp_over[6] = {0xe9, 0, 0, 0, 0, 0x90};
        memcpy(jmp_over + 1, &rel, 4);
        memcpy(image + 0xbbc3e0, jmp_over, sizeof(jmp_over));
        printf("Guest hooks: blur-kernel sample count clamp on (GPUTexUtil_GetRecursiveSampleParameters +0xbbc3e0)\n");
    } while (0);

    /* Panic/assert printer hook at 0x20b55b0. It is called with rdi=first string, esi=assert
     * id, rdx=second string and al=number of vector arguments, then either prints, returns
     * silently (mode 2) or aborts by writing to address 0. Recording the id and both strings
     * turns an otherwise silent failure (issue #148: id 0x104 and no output) into evidence.
     * thunks live in host VirtualAlloc memory that may be outside rel32 range, so the entry
     * uses a 14-byte absolute indirect jump padded to the 17-byte prologue boundary; the stub
     * replays those 17 bytes and resumes at 0x20b55c2. */
    do {
        const uint64_t assert_entry = 0x20b55b0, assert_resume = 0x20b55c2;
        static const unsigned char expected[17] = {
            0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
            0x48, 0x81, 0xec, 0x00, 0x01, 0x00
        };
        if (size < assert_entry + sizeof(expected)) {
            printf("Guest hooks: assert hook not installed (image too small)\n");
            break;
        }
        if (memcmp(image + assert_entry, expected, sizeof(expected)) != 0) {
            printf("Guest hooks: assert hook not installed (unmatched 0x20b55b0 signature)\n");
            break;
        }

        unsigned char *t = thunks + (import_count + 2) * 32;
        unsigned char *p = t;
        /* Preserve every argument register and XMM0-7: the printer saves all eight vector
         * registers when al != 0, so clobbering them would corrupt its variadic arguments. */
        *p++ = 0x50;                                        /* push rax */
        *p++ = 0x51;                                        /* push rcx */
        *p++ = 0x52;                                        /* push rdx */
        *p++ = 0x41; *p++ = 0x50;                           /* push r8  */
        *p++ = 0x41; *p++ = 0x51;                           /* push r9  */
        *p++ = 0x41; *p++ = 0x52;                           /* push r10 */
        *p++ = 0x41; *p++ = 0x53;                           /* push r11 */
        *p++ = 0x48; *p++ = 0x81; *p++ = 0xec;
        *p++ = 0x80; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; /* sub rsp, 0x80 */
        for (int i = 0; i < 8; ++i) {                       /* movaps [rsp+i*16], xmm_i */
            *p++ = 0x0f; *p++ = 0x29; *p++ = (unsigned char)(0x44 | (i << 3));
            *p++ = 0x24; *p++ = (unsigned char)(i * 16);
        }
        *p++ = 0x48; *p++ = 0x83; *p++ = 0xec; *p++ = 0x20; /* sub rsp, 0x20 (shadow space) */
        *p++ = 0x49; *p++ = 0x89; *p++ = 0xd0;              /* mov r8, rdx  (string 2) */
        *p++ = 0x48; *p++ = 0x89; *p++ = 0xf2;              /* mov rdx, rsi (assert id) */
        *p++ = 0x48; *p++ = 0x89; *p++ = 0xf9;              /* mov rcx, rdi (string 1) */
        /* Hook parameters are therefore (rcx=string 1, rdx=assert id, r8=string 2), which
         * matches guest_assert_hook(uint64_t s1, uint64_t id, uint64_t s2). */
        uint64_t hook = (uint64_t)(uintptr_t)guest_assert_hook;
        *p++ = 0x48; *p++ = 0xb8; memcpy(p, &hook, 8); p += 8; /* movabs rax, hook */
        *p++ = 0xff; *p++ = 0xd0;                           /* call rax */
        *p++ = 0x48; *p++ = 0x83; *p++ = 0xc4; *p++ = 0x20; /* add rsp, 0x20 */
        for (int i = 0; i < 8; ++i) {                       /* movaps xmm_i, [rsp+i*16] */
            *p++ = 0x0f; *p++ = 0x28; *p++ = (unsigned char)(0x44 | (i << 3));
            *p++ = 0x24; *p++ = (unsigned char)(i * 16);
        }
        *p++ = 0x48; *p++ = 0x81; *p++ = 0xc4;
        *p++ = 0x80; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; /* add rsp, 0x80 */
        *p++ = 0x41; *p++ = 0x5b;                           /* pop r11 */
        *p++ = 0x41; *p++ = 0x5a;                           /* pop r10 */
        *p++ = 0x41; *p++ = 0x59;                           /* pop r9  */
        *p++ = 0x41; *p++ = 0x58;                           /* pop r8  */
        *p++ = 0x5a;                                        /* pop rdx */
        *p++ = 0x59;                                        /* pop rcx */
        *p++ = 0x58;                                        /* pop rax */
        memcpy(p, image + assert_entry, sizeof(expected));   /* replay the prologue */
        p += sizeof(expected);
        uint64_t resume = (uint64_t)(uintptr_t)(image + assert_resume);
        *p++ = 0xff; *p++ = 0x25; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
        memcpy(p, &resume, 8);
        p += 8;

        if ((size_t)(p - t) > 6 * 32) {
            printf("Guest hooks: assert hook stub too large (%zu bytes); not installed\n",
                   (size_t)(p - t));
            break;
        }

        unsigned char entry_patch[17] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
        uint64_t thunk_addr = (uint64_t)(uintptr_t)t;
        memcpy(entry_patch + 6, &thunk_addr, 8);
        entry_patch[14] = 0x90;
        entry_patch[15] = 0x90;
        entry_patch[16] = 0x90;
        memcpy(image + assert_entry, entry_patch, sizeof(entry_patch));
        FlushInstructionCache(GetCurrentProcess(), image + assert_entry, sizeof(entry_patch));
        printf("Guest hooks: assert/panic hook on at 0x20b55b0 (stub %zu bytes at %p)\n",
               (size_t)(p - t), (void *)t);
    } while (0);

    /* Live engine clock (Phase 2): replace the dead null-assert block at 0x2018e3d with a jump
     * to a stub that calls bb_live_clock_tick(singleton). The stub writes the clamped dt into
     * the payload at [rbp-0x38] and resumes at 0x2018e6b. Installed after apply_patches() so it
     * wins over the static 60/90/Uncap block, and only after the exact CUSA03173 v1.09 bytes
     * are verified; otherwise the game keeps the XML patches and never sees the hook. */
    do {
        int live_mode = 0, live_target = 60, live_max_dt = 30;
        bbgpu_get_live_clock(&live_mode, &live_target, &live_max_dt);
        if (!live_mode) {
            bbgpu_set_live_clock_supported(0);
            printf("[LIVE CLOCK] Disabled by settings; using XML patches.\n");
            break;
        }
        static const unsigned char resume_expected[5] = {0x48, 0x8d, 0x75, 0xc0, 0xe8};
        static const unsigned char store_expected[3] = {0xc7, 0x45, 0xc8};
        const int wrong_title = s_game_title_id[0] && !is_supported_bloodborne_title(s_game_title_id);
        const int wrong_version = s_game_app_ver[0] && strcmp(s_game_app_ver, "01.09") != 0 && strcmp(s_game_app_ver, "01.00") != 0;
        if (size <= 0x2035000 || wrong_title || wrong_version ||
            memcmp(image + 0x2018e6b, resume_expected, sizeof(resume_expected)) != 0 ||
            memcmp(image + 0x2018e36, store_expected, sizeof(store_expected)) != 0) {
            fprintf(stderr, "[LIVE CLOCK] Incompatible eboot signature detected; falling back to XML patches.\n");
            fflush(stderr);
            bbgpu_set_live_clock_supported(0);
            break;
        }
        /* Slots +2..+7 hold the assert stub (up to 6*32 bytes) and +8 the aim camera jump. */
        unsigned char *t = thunks + (import_count + 9) * 32;
        const uintptr_t tick = (uintptr_t)bb_live_clock_tick;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xdf;             /* mov rdi, rbx */
        t[3] = 0x48; t[4] = 0xb8; memcpy(t + 5, &tick, 8); /* movabs rax, bb_live_clock_tick */
        t[13] = 0xff; t[14] = 0xd0;                        /* call rax */
        t[15] = 0xf3; t[16] = 0x0f; t[17] = 0x11; t[18] = 0x45; t[19] = 0xc8; /* movss [rbp-0x38], xmm0 */
        t[20] = 0xe9;                                      /* jmp 0x2018e6b */
        const int64_t rel = (int64_t)(image + 0x2018e6b) - (int64_t)(t + 25);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            fprintf(stderr, "[LIVE CLOCK] Incompatible eboot signature detected; falling back to XML patches.\n");
            fflush(stderr);
            bbgpu_set_live_clock_supported(0);
            break;
        }
        const int32_t rel32 = (int32_t)rel;
        memcpy(t + 21, &rel32, 4);
        unsigned char entry[14] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
        const uint64_t stub = (uint64_t)(uintptr_t)t;
        memcpy(entry + 6, &stub, 8);
        memcpy(image + 0x2018e3d, entry, sizeof(entry));
#ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), image + 0x2018e3d, sizeof(entry));
#else
        __builtin___clear_cache((char *)(image + 0x2018e3d), (char *)(image + 0x2018e3d + sizeof(entry)));
#endif
        static const unsigned char deltatime_cave[7] = {0x48, 0x8b, 0x05, 0xf1, 0x04, 0x4b, 0x00};
        g_live_clock_deltatime = (size > 0x5090007 && memcmp(image + 0x5090000, deltatime_cave, sizeof(deltatime_cave)) == 0);
        /* Uncap the internal guest frame pacer ceiling to 240 FPS (1/240 s = 0x3b888889).
         * This allows the presenter in driver.cpp (BbSettings::LiveFrameLimit) to freely pace
         * 90, 120, and Uncapped framerates without being clamped to 60 FPS by the guest engine. */
        static const uint32_t pacer_uncap = 0x3b888889;
        if (size > 0x20348bc &&
            image[0x2034840] == 0x41 && image[0x2034841] == 0xc7 &&
            image[0x203485a] == 0x41 && image[0x203485b] == 0xc7) {
            *(uint32_t *)(image + 0x2034845) = pacer_uncap;
            *(uint32_t *)(image + 0x203485f) = pacer_uncap;
            *(uint32_t *)(image + 0x2034883) = pacer_uncap;
            *(uint32_t *)(image + 0x20348b8) = pacer_uncap;
        }
        bbgpu_set_live_clock_supported(1);
        printf("[LIVE CLOCK] Hook installed at image+0x2018e3d (stub=%p, target=%d FPS, max dt 1/%d s, deltatime=%d)\n",
               (void *)t, live_target, live_max_dt, g_live_clock_deltatime);
    } while (0);

    protect(thunks, round_page((import_count + 16) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    start_watchdog();
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
    fail("entry unexpectedly returned");
}
