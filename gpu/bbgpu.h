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
#ifdef __cplusplus
}
#endif
#endif
