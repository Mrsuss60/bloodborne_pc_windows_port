/* Windows guest address space (runtime_memory.c):
 * Provides memfd + mmap(MAP_FIXED) semantics on Windows.
 * Protection bits are PROT_READ 1, PROT_WRITE 2, PROT_EXEC 4. */
#ifndef BB_WIN32_MEMORY_H
#define BB_WIN32_MEMORY_H
#ifdef _WIN32
#include <stdint.h>
#include <stddef.h>

/* Reserves [start,end) once as a placeholder: nothing else in the process can take it. */
int win_mem_space(uintptr_t start, uintptr_t end);
/* Pagefile-backed section of size bytes (the physical pool) and a host view of all of it. */
int win_mem_section(uint64_t size, void **backing);
/* Called for each piece of a view that had to be remapped around a removed range. */
typedef void (*WinMemRestore)(uintptr_t start, uintptr_t end);
/* Maps section offset phys at [a,b), replacing whatever was there. */
int win_mem_map(uintptr_t a, uintptr_t b, uint64_t phys, int prot, WinMemRestore restore);
/* Removes mappings in [a,b); the range stays reserved (no access). */
int win_mem_release(uintptr_t a, uintptr_t b, WinMemRestore restore);
int win_mem_protect(uintptr_t a, uintptr_t b, int prot);
/* Committed private read/write memory at exactly [a,b) (host-owned guest-visible memory). */
void *win_mem_private(uintptr_t a, uintptr_t b);
/* Lazy discard/zero-on-reuse for direct memory release without blocking memset. */
int win_mem_punch(uint64_t phys, uint64_t size);
/* Opt-in memory selftest for verification. */
void win_mem_run_selftest(void);
#endif
#endif
