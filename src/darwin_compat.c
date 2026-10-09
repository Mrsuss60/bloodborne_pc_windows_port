/* macOS: the guest TCB lives in a pthread TSD slot (see darwin_compat.h). */
#ifdef __APPLE__
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static pthread_key_t tcb_key;
static pthread_once_t tcb_once = PTHREAD_ONCE_INIT;
static void create_key(void) {
    if (pthread_key_create(&tcb_key, NULL)) { fputs("STOP: pthread_key_create failed\n", stderr); exit(21); }
}
unsigned runtime_darwin_tls_slot(void) {
    pthread_once(&tcb_once, create_key);
    return (unsigned)tcb_key;
}
void runtime_darwin_set_tcb(void *tcb) {
    if (pthread_setspecific((pthread_key_t)runtime_darwin_tls_slot(), tcb)) {
        fputs("STOP: pthread_setspecific failed\n", stderr); exit(21);
    }
}
uint64_t runtime_darwin_patch_tls_loads(unsigned char *code, size_t size) {
    /* A segment prefix (0x64 FS, 0x65 GS), then these 8 bytes: mov rax, [disp32 0]. */
    static const unsigned char load[] = {0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    const uint32_t disp = runtime_darwin_tls_slot() * 8;
    uint64_t patched = 0;
    if (size < 1 + sizeof(load)) return 0;
    unsigned char *at = code, *last = code + size - (1 + sizeof(load));
    while (at <= last) {
        if ((at[0] == 0x65 || at[0] == 0x64) && !memcmp(at + 1, load, sizeof(load))) {
            at[0] = 0x65;
            memcpy(at + 5, &disp, 4);
            ++patched;
            at += 1 + sizeof(load);
        } else {
            ++at;
        }
    }
    return patched;
}

#undef getrusage
int darwin_getrusage(int who, struct rusage *r) {
    if (who != RUSAGE_THREAD) return getrusage(who, r);
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) return -1;
    memset(r, 0, sizeof(*r));
    r->ru_utime.tv_sec = info.user_time.seconds; r->ru_utime.tv_usec = info.user_time.microseconds;
    r->ru_stime.tv_sec = info.system_time.seconds; r->ru_stime.tv_usec = info.system_time.microseconds;
    return 0;
}
ssize_t darwin_read_memory(void *out, const void *address, size_t size) {
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)address, size,
                               (mach_vm_address_t)(uintptr_t)out, &got) != KERN_SUCCESS) return -1;
    return (ssize_t)got;
}
int IsBadReadPtr(const void *address, size_t size) {
    uintptr_t at = (uintptr_t)address, end = at + size;
    if (!address || end < at) return 1;
    for (;;) {
        unsigned char probe;
        if (darwin_read_memory(&probe, (const void *)at, 1) != 1) return 1;
        uintptr_t next = (at | (uintptr_t)(vm_page_size - 1)) + 1;
        if (next >= end) return 0;
        at = next;
    }
}
#endif
