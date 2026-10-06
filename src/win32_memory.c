/* Windows guest address space manager.
 * Reserves the guest address range at startup as placeholders (VirtualAlloc2).
 * Mappings replace placeholders with views of the physical backing section (MapViewOfFile3).
 * Slices are tracked so partial unmap / overlay unmaps and remaps remnants. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32_memory.h"

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;
static HANDLE section;
static void *section_backing;
static uintptr_t space_start, space_end;

typedef struct { uintptr_t start, end; uint64_t phys; } View;
static View *views;
static size_t view_count, view_capacity;

static void report(const char *what, uintptr_t a, uintptr_t b) {
    fprintf(stderr, "Runtime: %s [%#llx, %#llx) failed: Windows error %lu\n", what,
            (unsigned long long)a, (unsigned long long)b, GetLastError());
}

int win_mem_space(uintptr_t start, uintptr_t end) {
    if (space_end) return 0;
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    if (!kernel) kernel = GetModuleHandleW(L"kernel32.dll");
    virtual_alloc2 = kernel ? (VirtualAlloc2Fn)(void *)GetProcAddress(kernel, "VirtualAlloc2") : NULL;
    map_view3 = kernel ? (MapViewOfFile3Fn)(void *)GetProcAddress(kernel, "MapViewOfFile3") : NULL;
    unmap_view2 = kernel ? (UnmapViewOfFile2Fn)(void *)GetProcAddress(kernel, "UnmapViewOfFile2") : NULL;
    if (!virtual_alloc2 || !map_view3 || !unmap_view2) {
        /* Fallback for pre-1803 systems */
        void *res = VirtualAlloc((void *)start, end - start, MEM_RESERVE, PAGE_NOACCESS);
        if (res != (void *)start) {
            report("reserving fallback guest address space", start, end);
            return -1;
        }
        space_start = start; space_end = end;
        return 0;
    }
    if (virtual_alloc2(GetCurrentProcess(), (void *)start, end - start, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                       PAGE_NOACCESS, NULL, 0) == (void *)start) {
        space_start = start; space_end = end;
        return 0;
    }

    /* Fallback: if single placeholder reservation fails (e.g. preexisting thread stack or module),
     * query memory and reserve all free regions in [start, end) as placeholders. */
    MEMORY_BASIC_INFORMATION info;
    uintptr_t at = start;
    int reserved_chunks = 0;
    while (at < end) {
        if (!VirtualQuery((void *)at, &info, sizeof(info))) break;
        uintptr_t chunk_start = (uintptr_t)info.BaseAddress;
        uintptr_t chunk_end = chunk_start + info.RegionSize;
        if (chunk_start < start) chunk_start = start;
        if (chunk_end > end) chunk_end = end;

        if (info.State == MEM_FREE) {
            SIZE_T chunk_size = chunk_end - chunk_start;
            void *p = virtual_alloc2(GetCurrentProcess(), (void *)chunk_start, chunk_size,
                                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
            if (p == (void *)chunk_start) reserved_chunks++;
        }
        if (chunk_end <= at) break;
        at = chunk_end;
    }
    if (reserved_chunks > 0) {
        space_start = start; space_end = end;
        return 0;
    }
    report("reserving the guest address space placeholder", start, end);
    return -1;
}

int win_mem_section(uint64_t size, void **backing) {
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                 (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!section) { report("creating the memory pool section", 0, size); return -1; }
    section_backing = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!section_backing) { report("mapping the memory pool", 0, size); return -1; }
    *backing = section_backing;
    return 0;
}

static DWORD page_mode(int prot) {
    switch (prot & 7) {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 2: case 3: return PAGE_READWRITE;
    case 4: return PAGE_EXECUTE;
    case 5: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

static size_t view_index(uintptr_t a) {
    size_t lo = 0, hi = view_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (views[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}

static int view_insert(View v) {
    if (view_count == view_capacity) {
        size_t capacity = view_capacity ? view_capacity * 2 : 256;
        View *next = realloc(views, capacity * sizeof(*views));
        if (!next) return -1;
        views = next; view_capacity = capacity;
    }
    size_t at = view_index(v.start);
    memmove(views + at + 1, views + at, (view_count - at) * sizeof(*views));
    views[at] = v; ++view_count;
    return 0;
}

static void view_erase(size_t at) {
    memmove(views + at, views + at + 1, (view_count - at - 1) * sizeof(*views));
    --view_count;
}

static int split_at(uintptr_t x) {
    if (x == space_start || x == space_end) return 0;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)x, &info, sizeof(info))) return -1;
    if ((uintptr_t)info.AllocationBase == x) return 0;
    if (info.State != MEM_RESERVE) { SetLastError(ERROR_INVALID_ADDRESS); return -1; }
    return VirtualFree((void *)x, info.RegionSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ? 0 : -1;
}

static int make_placeholder(uintptr_t a, uintptr_t b) {
    if (split_at(a) || split_at(b)) return -1;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)a, &info, sizeof(info))) return -1;
    if (info.RegionSize < b - a && !VirtualFree((void *)a, b - a, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) return -1;
    return 0;
}

static int map_view(uintptr_t a, uintptr_t b, uint64_t phys) {
    if (make_placeholder(a, b)) return -1;
    if (map_view3(section, GetCurrentProcess(), (void *)a, phys, b - a, MEM_REPLACE_PLACEHOLDER,
                  PAGE_READWRITE, NULL, 0) != (void *)a) return -1;
    return view_insert((View){a, b, phys});
}

static int inside(uintptr_t a, uintptr_t b) { return a < b && a >= space_start && b <= space_end; }

int win_mem_release(uintptr_t a, uintptr_t b, WinMemRestore restore) {
    if (!inside(a, b)) { SetLastError(ERROR_INVALID_ADDRESS); report("releasing", a, b); return -1; }
    for (size_t i = view_index(a); i < view_count && views[i].start < b;) {
        View v = views[i];
        if (!unmap_view2(GetCurrentProcess(), (void *)v.start, MEM_PRESERVE_PLACEHOLDER)) {
            report("unmapping", v.start, v.end);
            return -1;
        }
        view_erase(i);
        if (v.start < a) {
            if (map_view(v.start, a, v.phys)) { report("remapping remnant prefix", v.start, a); return -1; }
            if (restore) restore(v.start, a);
            ++i;
        }
        if (v.end > b) {
            if (map_view(b, v.end, v.phys + (b - v.start))) { report("remapping remnant suffix", b, v.end); return -1; }
            if (restore) restore(b, v.end);
        }
    }
    if (make_placeholder(a, b)) { report("reserving placeholder", a, b); return -1; }
    return 0;
}

int win_mem_map(uintptr_t a, uintptr_t b, uint64_t phys, int prot, WinMemRestore restore) {
    if (win_mem_release(a, b, restore)) return -1;
    if (map_view(a, b, phys)) { report("mapping view", a, b); return -1; }
    return (prot & 7) == 3 ? 0 : win_mem_protect(a, b, prot);
}

int win_mem_protect(uintptr_t a, uintptr_t b, int prot) {
    DWORD mode = page_mode(prot), old;
    for (size_t i = view_index(a); i < view_count && views[i].start < b; ++i) {
        uintptr_t start = views[i].start > a ? views[i].start : a;
        uintptr_t end = views[i].end < b ? views[i].end : b;
        if (!VirtualProtect((void *)start, end - start, mode, &old)) {
            report("protecting view slice", start, end);
            return -1;
        }
    }
    return 0;
}

void *win_mem_private(uintptr_t a, uintptr_t b) {
    if (!inside(a, b)) return NULL;
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t at = a; at < b; at = (uintptr_t)info.BaseAddress + info.RegionSize)
        if (!VirtualQuery((void *)at, &info, sizeof(info)) || info.State != MEM_RESERVE) return NULL;
    if (make_placeholder(a, b)) return NULL;
    return virtual_alloc2(GetCurrentProcess(), (void *)a, b - a, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                          PAGE_READWRITE, NULL, 0);
}

/* Lazy punch/discard: zero or reset pages in backing section without blocking memset */
int win_mem_punch(uint64_t phys, uint64_t size) {
    if (!section_backing) return 0;
    void *ptr = (char *)section_backing + phys;
    /* MEM_RESET notifies Windows memory manager that the pages are discarded and can be zeroed lazily */
    VirtualAlloc(ptr, size, MEM_RESET, PAGE_READWRITE);
    return 0;
}

void win_mem_run_selftest(void) {
    printf("[SELFTEST] Running win32_memory validation...\n");
    const uintptr_t test_addr = space_start + 0x1000000;
    const uint64_t test_size = 64 * 1024;
    int r = win_mem_map(test_addr, test_addr + test_size, 0, 3, NULL);
    if (r != 0) {
        printf("[SELFTEST] FAIL: initial map failed\n");
        return;
    }
    volatile uint32_t *data = (volatile uint32_t *)test_addr;
    data[0] = 0x12345678;
    data[test_size / 4 - 1] = 0x87654321;
    if (data[0] != 0x12345678 || data[test_size / 4 - 1] != 0x87654321) {
        printf("[SELFTEST] FAIL: data readback mismatch\n");
        return;
    }
    /* Test partial unmap in the middle */
    r = win_mem_release(test_addr + 16384, test_addr + 32768, NULL);
    if (r != 0) {
        printf("[SELFTEST] FAIL: middle release failed\n");
        return;
    }
    if (data[0] != 0x12345678 || data[test_size / 4 - 1] != 0x87654321) {
        printf("[SELFTEST] FAIL: remnant data corrupted after partial release\n");
        return;
    }
    win_mem_release(test_addr, test_addr + test_size, NULL);
    printf("[SELFTEST] PASS: win32_memory tests passed successfully.\n");
}
#endif
