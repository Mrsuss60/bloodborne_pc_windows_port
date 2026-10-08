#ifndef BB_WIN32_EXCEPTION_H
#define BB_WIN32_EXCEPTION_H
#ifdef _WIN32
#include <stdint.h>
#include <windows.h>

/* IsBadReadPtr deliberately faults on inaccessible memory and catches the AV
 * with SEH. A VEH runs before that handler: let Windows finish the probe rather
 * than treating it as a fatal game fault. Resolve the actual function extent
 * from unwind metadata instead of assuming an address or instruction length.
 * Missing metadata must not exempt unrelated host or guest faults. */
static inline int win_fault_is_read_probe(uintptr_t rip) {
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (!kernel) return 0;
    FARPROC probe = GetProcAddress(kernel, "IsBadReadPtr");
    if (!probe) return 0;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry((DWORD64)(uintptr_t)probe, &base, NULL);
    if (!entry) return 0;
    return rip >= base + entry->BeginAddress && rip < base + entry->EndAddress;
}
#endif
#endif
