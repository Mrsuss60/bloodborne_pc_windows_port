#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif
#ifdef __APPLE__
#include "darwin_compat.h"
#endif
#include "host_sync.h"

#ifdef _WIN32
/* runtime_setjmp/runtime_longjmp (runtime_host.c): every Win64 callee-saved register, and no
 * SEH unwinding, which cannot pass the guest frames in between (they have no unwind data). */
typedef struct { _Alignas(16) unsigned char registers[256]; } RuntimeRecoverBuf;
int runtime_setjmp(RuntimeRecoverBuf *buf) __attribute__((returns_twice));
__attribute__((noreturn)) void runtime_longjmp(RuntimeRecoverBuf *buf);
#define RUNTIME_RECOVER_SET(buf) runtime_setjmp(&(buf))
#define RUNTIME_RECOVER_JUMP(buf) runtime_longjmp(&(buf))
#else
#include <setjmp.h>
typedef sigjmp_buf RuntimeRecoverBuf;
#define RUNTIME_RECOVER_SET(buf) sigsetjmp(buf, 0)
#define RUNTIME_RECOVER_JUMP(buf) siglongjmp(buf, 1)
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread RuntimeRecoverBuf *runtime_fault_recover;

/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
/* Functions that top-level asm calls or defines by plain name (Darwin prefixes C symbols with '_'). */
#ifdef __APPLE__
#define ASM_NAME(name) __asm__(#name)
#else
#define ASM_NAME(name)
#endif
typedef void (ABI *GuestCallback)(void);

#define CHECK_LOW_ADDR(ptr) do { \
    uintptr_t _a = (uintptr_t)(ptr); \
    if (_a && _a >= (1ULL << 40)) { \
        fprintf(stderr, "AUDIT ERROR: address %p exceeds 1<<40 at %s:%d\n", (void *)_a, __FILE__, __LINE__); \
        fflush(stderr); \
    } \
} while (0)
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
void runtime_mutex_dump_info(void *f, void *handle);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end);
void runtime_memory_dump_recent_ops(void *file_handle);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_rwlock_dump_info(void *f, void *handle);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
void *runtime_thread_get_tcb(void);
ABI void restore_guest_fs(void) ASM_NAME(restore_guest_fs);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
int runtime_memory_reserve_space(void);
void *runtime_low_map(size_t size, int prot);
void *runtime_low_alloc(size_t size);
void *runtime_low_malloc(size_t size);
void *runtime_low_calloc(size_t nmemb, size_t size);
void *runtime_low_alloc_aligned(size_t size, size_t align);
void runtime_low_free(void *ptr);
void runtime_low_free_aligned(void *ptr);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);

#include <pthread.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    uint64_t *tcb;
    unsigned char *tls_block;
    HostThread host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
#ifdef _WIN32
    RuntimeRecoverBuf exit_jump;
    HANDLE win32_handle;
    DWORD win32_tid;
#else
    jmp_buf exit_jump;
#endif
    const char *blocked_on;
    uint64_t blocked_resource;
    uint64_t blocked_tick;
    uint32_t recent_imports[32];
    uint32_t recent_import_count;
    struct GuestThread *next;
} GuestThread;

GuestThread *runtime_thread_get_all(void);
GuestThread *runtime_thread_current(void);
void *runtime_thread_get_lock(void);
void runtime_thread_set_blocked(const char *what, uint64_t resource);
void runtime_thread_clear_blocked(void);
void runtime_notify_progress(void);
uint64_t runtime_audio_get_underruns(void);
#endif


