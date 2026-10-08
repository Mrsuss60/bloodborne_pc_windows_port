/* Standalone Windows regression: no game assets or GPU dependencies required. */
#include "win32_exception.h"
#include <assert.h>
#include <stdio.h>

static volatile LONG probe_faults;
static volatile LONG other_faults;
static int expect_probe;

static LONG WINAPI test_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (win_fault_is_read_probe(ep->ContextRecord->Rip)) {
        if (!expect_probe) ExitProcess(2);
        InterlockedIncrement(&probe_faults);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (expect_probe) ExitProcess(3); /* Would have reached bbport's fatal path. */
    InterlockedIncrement(&other_faults);
    /* Only resume the continuable exception raised explicitly below. */
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    unsigned char *pages = VirtualAlloc(NULL, 2 * info.dwPageSize,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(pages);
    DWORD old_protect;
    assert(VirtualProtect(pages + info.dwPageSize, info.dwPageSize,
                          PAGE_NOACCESS, &old_protect));
    PVOID handler = AddVectoredExceptionHandler(1, test_veh);
    assert(handler);

    expect_probe = 1;
    assert(!IsBadReadPtr(pages, info.dwPageSize));
    assert(probe_faults == 0);
    assert(IsBadReadPtr(pages + info.dwPageSize, 1));
    assert(probe_faults > 0);
    LONG before = probe_faults;
    assert(IsBadReadPtr(pages + info.dwPageSize - 1, 2));
    assert(probe_faults > before); /* Cross-page read also reaches Windows SEH. */

    expect_probe = 0;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    assert(other_faults == 1); /* An unrelated host AV is not exempted. */
    assert(!win_fault_is_read_probe((uintptr_t)pages));
    assert(!win_fault_is_read_probe(0));
    assert(!win_fault_is_read_probe(UINTPTR_MAX));

    assert(RemoveVectoredExceptionHandler(handler));
    assert(VirtualFree(pages, 0, MEM_RELEASE));
    puts("Windows read-probe exception tests passed");
    return 0;
}
