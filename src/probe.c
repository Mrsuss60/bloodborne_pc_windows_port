/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include "win32_exception.h"
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/uio.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach/mach.h>
/* Darwin's machine context: the x86-64 thread state (also under Rosetta 2). */
#define CONTEXT_RIP(uc) ((uc)->uc_mcontext->__ss.__rip)
#define CONTEXT_RBP(uc) ((uc)->uc_mcontext->__ss.__rbp)
#define CONTEXT_RDI(uc) ((uc)->uc_mcontext->__ss.__rdi)
#else
#include <malloc.h>
#include <ucontext.h>
#include <sys/syscall.h>
#define CONTEXT_RIP(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#define CONTEXT_RBP(uc) ((uc)->uc_mcontext.gregs[REG_RBP])
#define CONTEXT_RDI(uc) ((uc)->uc_mcontext.gregs[REG_RDI])
#endif
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
static size_t round_to_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
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
#ifdef _WIN32
static volatile uint64_t g_last_progress_tick = 0;
static volatile int g_watchdog_running = 1;
#endif

void runtime_notify_progress(void) {
#ifdef _WIN32
    g_last_progress_tick = GetTickCount64();
#endif
}

ABI void restore_guest_fs(void) {
#ifdef _WIN32
    void *tcb = runtime_thread_get_tcb();
    if (tcb) {
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

ABI void *pre_import_hook(uint32_t index, uint64_t *args, void *caller) ASM_NAME(pre_import_hook);
ABI void post_import_hook(uint32_t index, uint64_t ret_val) ASM_NAME(post_import_hook);
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

void common_dispatch(void) ASM_NAME(common_dispatch);
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

static char g_run_timestamp[32];
#ifdef _WIN32 /* the watchdog thread below */
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
#endif

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
        ExitProcess(0);
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
"   push %r12\n"
"   push %r13\n"
"   push %r14\n"
"   push %r15\n"
"   mov %gs:0x8, %r12\n"
"   mov %gs:0x10, %r13\n"
"   mov %rcx, %r14\n" /* save entry */
"   mov %rdx, %r15\n" /* save arg0 */
"   mov %r8, %r11\n"  /* save arg1 */
"   mov %r9, %rsp\n"
"   and $-16, %rsp\n"
"   mov %rsp, %gs:0x8\n"
"   sub $0x800000, %r9\n"
"   mov %r9, %gs:0x10\n"
"   sub $0x20, %rsp\n"
"   mov %r11, 0x18(%rsp)\n" /* preserve arg1 across call */
"   call runtime_thread_get_tcb\n"
"   wrfsbase %rax\n"
"   mov %r14, %r11\n"
"   mov %r15, %rdi\n"
"   mov 0x18(%rsp), %rsi\n"
"   call *%r11\n"
"   add $0x20, %rsp\n"
"   mov %r12, %gs:0x8\n"
"   mov %r13, %gs:0x10\n"
"   lea -0x20(%rbp), %rsp\n"
"   pop %r15\n"
"   pop %r14\n"
"   pop %r13\n"
"   pop %r12\n"
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

static void write_crash_dump(EXCEPTION_POINTERS *ep, void *fault_addr, uintptr_t rip, DWORD code) {
    char crash_path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("out", NULL);
    snprintf(crash_path, sizeof(crash_path), "out/crash_%04d%02d%02d_%02d%02d%02d.log",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(crash_path, "w");
    if (!f) f = fopen("crash.log", "w");
    if (!f) return;

    uintptr_t off = (rip >= (uintptr_t)image && rip < (uintptr_t)image + (image_size ? image_size : 0x20000000))
                    ? (rip - (uintptr_t)image) : 0;
    fprintf(f, "================================================================================\n");
    fprintf(f, "=== BLOODBORNE CRASH DUMP ===\n");
    fprintf(f, "================================================================================\n\n");
    fprintf(f, "Exception Code: 0x%08lx\n", code);
    fprintf(f, "Fault Address:  %p\n", fault_addr);
    if (off) fprintf(f, "Faulting RIP:   %p (guest eboot.bin offset 0x%" PRIxPTR ")\n\n", (void *)rip, off);
    else fprintf(f, "Faulting RIP:   %p (host address)\n\n", (void *)rip);

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
    void *cur_fs = NULL;
    __asm__ __volatile__("rdfsbase %0" : "=r"(cur_fs));
    fprintf(f, "RIP=%016llx EFLAGS=%08lx FS_BASE=%p GS_SELECTOR=0x%x\n\n",
            (unsigned long long)c->Rip, (unsigned long)c->EFlags, cur_fs, (unsigned int)c->SegGs);

    fprintf(f, "--- STACK WALK (16 FRAMES) ---\n");
    uintptr_t rsp = c->Rsp;
    if (rsp && rsp < (1ULL << 46)) {
        uintptr_t *stack_ptr = (uintptr_t *)rsp;
        if (!IsBadReadPtr(stack_ptr, 16 * sizeof(uintptr_t))) {
            for (int i = 0; i < 16; ++i) {
                uintptr_t val = stack_ptr[i];
                char loc[256];
                if (val >= (uintptr_t)image && val < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
                    uintptr_t val_off = val - (uintptr_t)image;
                    int found_mod = 0;
                    for (uint64_t m = 0; m < module_count; ++m) {
                        if (val_off >= modules[m].base && val_off < modules[m].base + modules[m].size) {
                            snprintf(loc, sizeof(loc), "module %" PRIu64 " +0x%" PRIxPTR, m, val_off - modules[m].base);
                            found_mod = 1;
                            break;
                        }
                    }
                    if (!found_mod) snprintf(loc, sizeof(loc), "guest eboot.bin +0x%" PRIxPTR, val_off);
                } else {
                    snprintf(loc, sizeof(loc), "host / stack data");
                }
                fprintf(f, "  [frame +0x%02x] %016llx (%s)\n", i * 8, (unsigned long long)val, loc);
            }
        }
    }

    fprintf(f, "\n--- GUEST RBP FRAME CHAIN ---\n");
    uintptr_t rbp = c->Rbp;
    for (int frame = 0; frame < 16 && rbp && rbp < (1ULL << 46); ++frame) {
        if (IsBadReadPtr((void *)rbp, 16)) break;
        uintptr_t *rbp_ptr = (uintptr_t *)rbp;
        uintptr_t next_rbp = rbp_ptr[0];
        uintptr_t ret_addr = rbp_ptr[1];
        if (!ret_addr) break;
        char loc[256];
        if (ret_addr >= (uintptr_t)image && ret_addr < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
            uintptr_t val_off = ret_addr - (uintptr_t)image;
            snprintf(loc, sizeof(loc), "guest eboot.bin +0x%" PRIxPTR, val_off);
        } else {
            snprintf(loc, sizeof(loc), "host %p", (void *)ret_addr);
        }
        fprintf(f, "  [rbp frame %2d] ret=%016llx (%s)\n", frame, (unsigned long long)ret_addr, loc);
        if (next_rbp <= rbp) break;
        rbp = next_rbp;
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
    if (gpu_enabled) bbgpu_dump_breadcrumbs(f);
    fflush(f);
    fclose(f);
    char note[256];
    snprintf(note, sizeof(note), "Crash details written to %s\n", crash_path);
    veh_write(note);
}

static LONG WINAPI win_veh_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        void *fault_addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t rip = ep->ContextRecord->Rip;
        // Save the 128-byte System V AMD64 Red Zone [RSP-128 .. RSP] before any handler runs.
        // Windows kernel exception dispatch (KiUserExceptionDispatch) pushes its frame below RSP,
        // which clobbers variables (like -0x60(%rsp)) used by PS4 guest functions.
        char red_zone_backup[128];
        uintptr_t red_zone_rsp = ep->ContextRecord->Rsp;
        int has_red_zone = 0;
        if (red_zone_rsp >= 128 && red_zone_rsp < (1ULL << 46)) {
            memcpy(red_zone_backup, (void *)(red_zone_rsp - 128), 128);
            has_red_zone = 1;
        }

        unsigned char *ip = (unsigned char *)rip;
        if (rip >= 0x10000 && !IsBadReadPtr((const void *)rip, 1) && ip[0] == 0x64) {
            void *tcb = runtime_thread_get_tcb();
            if (tcb) {
                __asm__ __volatile__("wrfsbase %0" : : "r"(tcb));
                if (ip[1] == 0x48 && ip[2] == 0x8b && ip[3] == 0x04 && ip[4] == 0x25 &&
                    ip[5] == 0x00 && ip[6] == 0x00 && ip[7] == 0x00 && ip[8] == 0x00) {
                    ep->ContextRecord->Rax = *(DWORD64 *)tcb;
                    ep->ContextRecord->Rip += 9;
                }
                if (has_red_zone) memcpy((void *)(red_zone_rsp - 128), red_zone_backup, 128);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (gpu_enabled && bbgpu_handle_fault(ep, fault_addr)) {
            if (has_red_zone) memcpy((void *)(red_zone_rsp - 128), red_zone_backup, 128);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rip >= (uintptr_t)image && rip < (uintptr_t)image + (image_size ? image_size : 0x20000000)) {
            uintptr_t off = rip - (uintptr_t)image;
            // Function 0x28ce7b0 is the particle/blood stream vertex generator.
            // On Windows, if VRAM pressure or page-tracking clobbers its System V Red Zone (-0x60(%rsp)),
            // or if vertex stream pointers are uninitialized, safely unroll the frame and return to caller.
            if (off >= 0x28ce7ba && off < 0x28ceeb8) {
                uintptr_t rsp = ep->ContextRecord->Rsp;
                ep->ContextRecord->Rbx = *(DWORD64 *)(rsp + 0x30);
                ep->ContextRecord->R12 = *(DWORD64 *)(rsp + 0x38);
                ep->ContextRecord->R13 = *(DWORD64 *)(rsp + 0x40);
                ep->ContextRecord->R14 = *(DWORD64 *)(rsp + 0x48);
                ep->ContextRecord->R15 = *(DWORD64 *)(rsp + 0x50);
                ep->ContextRecord->Rbp = *(DWORD64 *)(rsp + 0x58);
                ep->ContextRecord->Rip = *(DWORD64 *)(rsp + 0x60);
                ep->ContextRecord->Rsp = rsp + 0x68;
                return EXCEPTION_CONTINUE_EXECUTION;
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
        write_crash_dump(ep, fault_addr, rip, code);
        char line[512];
        if (rip - (uintptr_t)image < 0x10000000)
            snprintf(line, sizeof(line), "Guest fault (signal 11) at guest offset 0x%llx, address %p\n",
                     (unsigned long long)(rip - (uintptr_t)image), fault_addr);
        else
            snprintf(line, sizeof(line), "Fault at RIP %p, address %p\n", (void *)rip, fault_addr);
        veh_write(line);
        snprintf(line, sizeof(line), "RAX=%p RBX=%p RCX=%p RDX=%p RSI=%p RDI=%p RBP=%p RSP=%p\n",
                 (void *)ep->ContextRecord->Rax, (void *)ep->ContextRecord->Rbx,
                 (void *)ep->ContextRecord->Rcx, (void *)ep->ContextRecord->Rdx,
                 (void *)ep->ContextRecord->Rsi, (void *)ep->ContextRecord->Rdi,
                 (void *)ep->ContextRecord->Rbp, (void *)ep->ContextRecord->Rsp);
        veh_write(line);
        void *cur_fs = NULL;
        __asm__ __volatile__("rdfsbase %0" : "=r"(cur_fs));
        snprintf(line, sizeof(line), "FS_BASE=%p GS_SELECTOR=0x%x\n", cur_fs, (unsigned int)ep->ContextRecord->SegGs);
        veh_write(line);
        if (gpu_enabled) bbgpu_dump_guest_writes(ep);
        TerminateProcess(GetCurrentProcess(), 128 + 11);
    } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ||
               code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
               code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */ ||
               code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */) {
        uintptr_t rip = ep->ContextRecord->Rip;
        write_crash_dump(ep, (void *)rip, rip, code);
        char line[512];
        if (rip - (uintptr_t)image < 0x10000000)
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
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top) ASM_NAME(enter_on_stack);
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
    uintptr_t rip = (uintptr_t)CONTEXT_RIP(uc);
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
    uintptr_t rip=(uintptr_t)CONTEXT_RIP(uc), rbp=(uintptr_t)CONTEXT_RBP(uc);
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)CONTEXT_RDI(uc));
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
#ifdef __APPLE__
        if (!rbp || darwin_read_memory(frame,(void *)rbp,sizeof(frame))!=(ssize_t)sizeof(frame)) break;
#else
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
#endif
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
#ifdef __APPLE__
    /* Every other thread dumps its own frames (SIGUSR2). */
    thread_act_array_t list; mach_msg_type_number_t count=0;
    mach_port_t self_port=mach_thread_self();
    if (task_threads(mach_task_self(),&list,&count)==KERN_SUCCESS) {
        for (mach_msg_type_number_t i=0;i<count;++i) {
            pthread_t other=list[i]!=self_port ? pthread_from_mach_thread_np(list[i]) : NULL;
            if (other) { pthread_kill(other,SIGUSR2); usleep(20000); }
        }
    }
#else
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
#endif
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
#ifdef __APPLE__
/* link_libc.py/link_modules.py rewrite the eboot's `mov rax, fs:[0]` to `mov rax, gs:[0]`
 * (Linux: GS base = guest TCB). macOS points GS at the pthread TSD array, so the
 * displacement becomes the TSD slot that holds the guest TCB (darwin_compat.c). */
static void patch_tls_loads(const Segment *segments, uint64_t ns) {
    static const unsigned char gs_load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    static const unsigned char fs_load[]={0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t disp=runtime_darwin_tls_slot()*8;
    uint64_t patched=0;
    for (uint64_t i=0;i<ns;++i) {
        if (!(segments[i].flags&1)) continue;
        unsigned char *at=image+segments[i].address, *end=at+segments[i].size;
        while (at+sizeof(gs_load)<=end) {
            unsigned char *gs=memmem(at,(size_t)(end-at),gs_load,sizeof(gs_load));
            unsigned char *fs=memmem(at,(size_t)(end-at),fs_load,sizeof(fs_load));
            unsigned char *hit=!gs ? fs : !fs ? gs : gs<fs ? gs : fs;
            if (!hit) break;
            hit[0]=0x65; memcpy(hit+5,&disp,4);
            ++patched; at=hit+sizeof(gs_load);
        }
    }
    printf("Thread pointer loads: %" PRIu64 " pointed at TSD slot %u\n",patched,(unsigned)(disp/8));
}
#endif
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifndef _WIN32
    puts("Runtime: restarting through run.sh");
#ifdef __APPLE__
    for (int fd=getdtablesize()-1; fd>=3; --fd) close(fd);
#else
    syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
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
        ExitProcess(0);
    } else {
        fail("runtime_restart: CreateProcess failed");
    }
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    runtime_memory_reserve_space();
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
#if !defined(_WIN32) && !defined(__APPLE__)
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits.
       Windows and macOS have no such heap; those objects come from runtime_low_*. */
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
    int cpuinfo[4] = {0};
    __cpuid_count(7, 0, cpuinfo[0], cpuinfo[1], cpuinfo[2], cpuinfo[3]);
    if (!(cpuinfo[1] & 1)) {
        fprintf(stderr, "Warning: CPU does not support FSGSBASE instructions (wrfsbase/rdfsbase).\n");
    }
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, win_veh_handler);
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
    image = allocate(round_to_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
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
    unsigned char *thunks = allocate(round_to_page((import_count + 1) * 32));
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
#ifdef __APPLE__
    patch_tls_loads(segments, ns);
#endif
    protect(thunks, round_to_page((import_count + 1) * 32), 5);
    protect(image, round_to_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_to_page(segments[i].size), (unsigned)segments[i].flags);
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
