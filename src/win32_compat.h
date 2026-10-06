/* Windows host: file system, time zone, CPU time, randomness and naming pieces. */
#ifndef BB_WIN32_COMPAT_H
#define BB_WIN32_COMPAT_H
#ifdef _WIN32
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* POSIX calls with a Windows implementation. */
#define mkdir(path, mode) runtime_win_mkdir(path, mode)
#define rename(from, to) runtime_win_rename(from, to)
#define fsync(fd) _commit(fd)
#define realpath(path, resolved) _fullpath(resolved, path, PATH_MAX)
#define pread runtime_win_pread
#define pwrite runtime_win_pwrite
int runtime_win_mkdir(const char *path, int mode);
int runtime_win_rename(const char *from, const char *to);
int64_t runtime_win_pread(int fd, void *buffer, size_t size, int64_t offset);
int64_t runtime_win_pwrite(int fd, const void *buffer, size_t size, int64_t offset);
int runtime_win_remove_tree(const char *path);

/* Time: local UTC offset, CPU times, randomness. */
int64_t runtime_utc_offset(time_t when);
void runtime_cpu_times(int thread, int64_t *user_us, int64_t *system_us);
int runtime_random(void *buffer, size_t size);

/* Threads: guest TCB in a TEB TLS slot. */
uint32_t runtime_win_tls_slot(void);
void runtime_win_set_tcb(void *tcb);
void runtime_win_set_thread_name(const char *name);

/* Networking helpers without winsock headers. */
int runtime_win_inet_pton4(const char *src, void *dst);
const char *runtime_win_inet_ntop4(const void *src, char *dst, uint32_t size);
#endif
#endif
