/* Host rwlocks (pthread rwlocks on Linux, SRW locks on Windows) behind the PS4
   pointer-to-handle ABI. The registry protects lifetime, static initialization and
   ownership; waiting happens outside it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef struct { int64_t seconds, nanoseconds; } GuestTime;

#ifdef _WIN32
typedef SRWLOCK NativeRwlock;
static int native_init(NativeRwlock *l) { InitializeSRWLock(l); return 0; }
static int native_destroy(NativeRwlock *l) { (void)l; return 0; }
static int native_try(NativeRwlock *l, int writer) {
    return (writer ? TryAcquireSRWLockExclusive(l) : TryAcquireSRWLockShared(l)) ? 0 : EBUSY;
}
static int native_lock(NativeRwlock *l, int writer) {
    if (writer) AcquireSRWLockExclusive(l); else AcquireSRWLockShared(l);
    return 0;
}
/* SRW locks have no timed acquire: retry until the realtime deadline. */
static int native_timedlock(NativeRwlock *l, int writer, const GuestTime *time) {
    uint64_t deadline = (uint64_t)time->seconds * 1000000000ull + (uint64_t)time->nanoseconds;
    for (unsigned attempt = 0; ; ++attempt) {
        if (!native_try(l, writer)) return 0;
        if (host_realtime_ns() >= deadline) return ETIMEDOUT;
        if (attempt < 32) YieldProcessor();
        else SwitchToThread();
    }
}
static int native_unlock(NativeRwlock *l, int writer) {
    if (writer) ReleaseSRWLockExclusive(l); else ReleaseSRWLockShared(l);
    return 0;
}
#else
typedef pthread_rwlock_t NativeRwlock;
static int native_init(NativeRwlock *l) { return pthread_rwlock_init(l, NULL); }
static int native_destroy(NativeRwlock *l) { return pthread_rwlock_destroy(l); }
static int native_try(NativeRwlock *l, int writer) { return writer ? pthread_rwlock_trywrlock(l) : pthread_rwlock_tryrdlock(l); }
static int native_lock(NativeRwlock *l, int writer) { return writer ? pthread_rwlock_wrlock(l) : pthread_rwlock_rdlock(l); }
static int native_timedlock(NativeRwlock *l, int writer, const GuestTime *time) {
    struct timespec deadline = {.tv_sec = (time_t)time->seconds, .tv_nsec = (long)time->nanoseconds};
    return writer ? pthread_rwlock_timedwrlock(l, &deadline) : pthread_rwlock_timedrdlock(l, &deadline);
}
static int native_unlock(NativeRwlock *l, int writer) { (void)writer; return pthread_rwlock_unlock(l); }
#endif

#define RWLOCK_MAGIC 0x52574C4B
#define MAX_HOLDERS_PER_RWLOCK 8

typedef struct Holder {
    HostThreadId thread;
    unsigned readers, writer, pending;
} Holder;

typedef struct Rwlock {
    uint32_t magic;
    HostMutex lock;
    NativeRwlock native;
    Holder holders[MAX_HOLDERS_PER_RWLOCK];
    unsigned inflight;
    struct Rwlock *next;
} Rwlock;

static HostMutex registry_lock = HOST_MUTEX_INIT;
static Rwlock *registry;
static size_t created, reads, writes, unlocks;

static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}

/* All helpers ending in _locked require registry_lock. */
static Rwlock *find_locked(Rwlock *handle) {
    for (Rwlock *r=registry; r; r=r->next) if (r==handle) return r;
    return NULL;
}

static int create_locked(Rwlock **out) {
    Rwlock *r=runtime_low_calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    CHECK_LOW_ADDR(r);
    int e=native_init(&r->native);
    if (e) { runtime_low_free(r); return e; }
    host_mutex_init(&r->lock);
    r->magic = RWLOCK_MAGIC;
    r->next=registry; registry=r; *out=r; ++created; return 0;
}

static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) { restore_guest_fs(); return error(EINVAL); }
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    host_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out);
    host_unlock(&registry_lock);
    restore_guest_fs();
    return error(e);
}

enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;

    Rwlock *r = *handle;
    if (!r || r->magic != RWLOCK_MAGIC) {
        host_lock(&registry_lock);
        int e = 0;
        if (!*handle) e = create_locked(handle);
        r = e ? NULL : find_locked(*handle);
        host_unlock(&registry_lock);
        if (!r) return error(e ? e : EINVAL);
    }

    HostThreadId self = host_thread_id();
    Holder *h = NULL, *free_h = NULL;
    host_lock(&r->lock);
    for (int i = 0; i < MAX_HOLDERS_PER_RWLOCK; ++i) {
        if (host_thread_equal(r->holders[i].thread, self)) {
            h = &r->holders[i];
            break;
        }
        if (!free_h && !r->holders[i].thread) {
            free_h = &r->holders[i];
        }
    }
    if (h && h->writer) { host_unlock(&r->lock); restore_guest_fs(); return error(attempt ? EBUSY : EDEADLK); }
    if (h && h->readers && writer) { host_unlock(&r->lock); restore_guest_fs(); return error(attempt ? EBUSY : EDEADLK); }
    if (h && h->readers && !writer) {
        /* Recursive read acquire: already holding shared lock, avoid native lock re-acquire */
        ++h->readers;
        __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
        host_unlock(&r->lock);
        restore_guest_fs();
        return 0;
    }
    if (h && h->readers == UINT_MAX) { host_unlock(&r->lock); restore_guest_fs(); return error(EAGAIN); }
    if (!h) {
        h = free_h;
        if (!h) {
            host_unlock(&r->lock);
            restore_guest_fs();
            return error(ENOMEM);
        }
        *h = (Holder){.thread = self};
    }
    ++r->inflight; ++h->pending;
    host_unlock(&r->lock);

    /* Timed operations first try the lock, matching POSIX timeout validation. */
    runtime_thread_set_blocked(writer ? "rwlock_write" : "rwlock_read", (uintptr_t)*handle);
    int e = 0;
    if (attempt || timed) e=native_try(&r->native,writer);
    else e=native_lock(&r->native,writer);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000 || time->seconds<0) e=EINVAL;
        else e=native_timedlock(&r->native,writer,time);
    }
    runtime_thread_clear_blocked();

    host_lock(&r->lock);
    --r->inflight; --h->pending;
    if (!e) {
        if (writer) { h->writer = 1; __atomic_add_fetch(&writes, 1, __ATOMIC_RELAXED); }
        else { ++h->readers; __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED); }
    } else {
        if (!h->readers && !h->writer && !h->pending) {
            h->thread = 0;
        }
    }
    host_unlock(&r->lock);
    restore_guest_fs();
    return error(e);
}

static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }

static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Rwlock *r = *handle;
    if (!r || r->magic != RWLOCK_MAGIC) {
        host_lock(&registry_lock);
        r = find_locked(*handle);
        host_unlock(&registry_lock);
        if (!r) return error(EINVAL);
    }
    HostThreadId self = host_thread_id();
    Holder *h = NULL;
    host_lock(&r->lock);
    for (int i = 0; i < MAX_HOLDERS_PER_RWLOCK; ++i) {
        if (host_thread_equal(r->holders[i].thread, self)) {
            h = &r->holders[i];
            break;
        }
    }
    if (!h || (!h->readers && !h->writer)) { host_unlock(&r->lock); return error(EPERM); }
    int is_writer = h->writer;
    if (!is_writer && h->readers > 1) {
        --h->readers;
        __atomic_add_fetch(&unlocks, 1, __ATOMIC_RELAXED);
        host_unlock(&r->lock);
        restore_guest_fs();
        return 0;
    }
    int e=native_unlock(&r->native, is_writer);
    if (!e) {
        if (is_writer) h->writer = 0;
        else --h->readers;
        __atomic_add_fetch(&unlocks, 1, __ATOMIC_RELAXED);
        if (!h->readers && !h->writer && !h->pending) {
            h->thread = 0;
        }
    }
    host_unlock(&r->lock);
    restore_guest_fs();
    return error(e);
}

static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) { restore_guest_fs(); return error(EINVAL); }
    host_lock(&registry_lock);
    if (!*handle) { host_unlock(&registry_lock); restore_guest_fs(); return 0; }
    Rwlock *r=find_locked(*handle);
    if (!r) { host_unlock(&registry_lock); restore_guest_fs(); return error(EINVAL); }
    int has_holders = 0;
    host_lock(&r->lock);
    for (int i = 0; i < MAX_HOLDERS_PER_RWLOCK; ++i) {
        if (r->holders[i].readers || r->holders[i].writer || r->holders[i].pending) {
            has_holders = 1;
            break;
        }
    }
    if (r->inflight || has_holders) {
        host_unlock(&r->lock);
        host_unlock(&registry_lock);
        restore_guest_fs();
        return error(EBUSY);
    }
    host_unlock(&r->lock);
    int e=native_destroy(&r->native);
    if (!e) {
        Rwlock **link=&registry;
        while (*link && *link!=r) link=&(*link)->next;
        if (*link) *link=r->next;
        r->magic=0;
        runtime_low_free(r);
        *handle=(Rwlock *)(uintptr_t)1;
    }
    host_unlock(&registry_lock);
    restore_guest_fs();
    return error(e);
}

void runtime_rwlock_dump_info(void *f_ptr, void *handle) {
    FILE *f = (FILE *)f_ptr;
    if (!handle || !f) return;
    host_lock(&registry_lock);
    Rwlock *r = find_locked((Rwlock *)handle);
    if (!r) {
        fprintf(f, "    [rwlock %p: not found in registry]\n", handle);
        host_unlock(&registry_lock);
        return;
    }
    fprintf(f, "    [rwlock %p: inflight=%u]\n", handle, r->inflight);
    for (int i = 0; i < MAX_HOLDERS_PER_RWLOCK; ++i) {
        Holder *h = &r->holders[i];
        if (!h->thread && !h->readers && !h->writer && !h->pending) continue;
        const char *th_name = "unknown";
        for (GuestThread *t = runtime_thread_get_all(); t; t = t->next) {
#ifdef _WIN32
            if (t->win32_tid == (DWORD)h->thread) {
                th_name = t->name;
                break;
            }
#else
            if (host_thread_equal(t->host, h->thread)) {
                th_name = t->name;
                break;
            }
#endif
        }
        fprintf(f, "      holder thread '%s' (%llu): readers=%u, writer=%u, pending=%u\n",
                th_name, (unsigned long long)h->thread, h->readers, h->writer, h->pending);
    }
    host_unlock(&registry_lock);
}

uintptr_t runtime_rwlock_resolve(const char *name) {
    if (!strcmp(name,"6ULAa0fq4jA#p#J")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#p#J")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#p#J")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#p#J")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#p#J")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#p#J")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#p#J")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#p#J")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#p#J")) return (uintptr_t)rw_timedwrite;
    return 0;
}

void runtime_rwlock_report(void) {
    host_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",created,reads,writes,unlocks);
    host_unlock(&registry_lock);
}
