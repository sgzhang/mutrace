/*-*- Mode: C; c-basic-offset: 8 -*-*/

#define _GNU_SOURCE

#include <pthread.h>
#include <execinfo.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

/* FIXMES:
 *
 *   - we probably should cover rwlocks, too
 *
 */

typedef void (*fnptr_t)(void);

struct mutex_info {
        pthread_mutex_t *mutex;

        bool is_recursive:1;
        bool broken:1;

        unsigned n_lock_level;

        pid_t last_owner;

        unsigned n_locked;
        unsigned n_owner_changed;
        unsigned n_contended;

        uint64_t nsec_locked_total;
        uint64_t nsec_locked_max;

        struct timespec timestamp;
        char *stacktrace;

        unsigned id;

        struct mutex_info *next;
};

static unsigned long hash_size = 557;
static unsigned long frames_max = 16;

static volatile unsigned n_broken = 0;
static volatile unsigned n_collisions = 0;
static volatile unsigned n_self_contended = 0;

static unsigned show_n_locked_min = 1;
static unsigned show_n_owner_changed_min = 2;
static unsigned show_n_contended_min = 0;

static int (*real_pthread_mutex_init)(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) = NULL;
static int (*real_pthread_mutex_destroy)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_lock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_mutex_timedlock)(pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_wait)(pthread_cond_t *cond, pthread_mutex_t *mutex) = NULL;
static int (*real_pthread_cond_timedwait)(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) = NULL;
static void (*real_exit)(int status) __attribute__((noreturn)) = NULL;
static void (*real__Exit)(int status) __attribute__((noreturn)) = NULL;

static struct mutex_info **alive_mutexes = NULL, **dead_mutexes = NULL;
static pthread_mutex_t *mutexes_lock = NULL;

static __thread bool recursive = false;

static void setup(void) __attribute ((constructor));
static void shutdown(void) __attribute ((destructor));

/* dlsym() violates ISO C, so confide the breakage into this function
 * to avoid warnings. */

static inline fnptr_t dlsym_fn(void *handle, const char *symbol) {
    return (fnptr_t) (long) dlsym(handle, symbol);
}

static pid_t _gettid(void) {
        return (pid_t) syscall(SYS_gettid);
}

static void setup(void) {
        const char *e;
        pthread_mutex_t *m, *last;
        int r;

        real_pthread_mutex_init = dlsym_fn(RTLD_NEXT, "pthread_mutex_init");
        real_pthread_mutex_destroy = dlsym_fn(RTLD_NEXT, "pthread_mutex_destroy");
        real_pthread_mutex_lock = dlsym_fn(RTLD_NEXT, "pthread_mutex_lock");
        real_pthread_mutex_trylock = dlsym_fn(RTLD_NEXT, "pthread_mutex_trylock");
        real_pthread_mutex_timedlock = dlsym_fn(RTLD_NEXT, "pthread_mutex_timedlock");
        real_pthread_mutex_unlock = dlsym_fn(RTLD_NEXT, "pthread_mutex_unlock");
        real_pthread_cond_wait = dlsym_fn(RTLD_NEXT, "pthread_cond_wait");
        real_pthread_cond_timedwait = dlsym_fn(RTLD_NEXT, "pthread_cond_timedwait");
        real_exit = dlsym_fn(RTLD_NEXT, "exit");
        real__Exit = dlsym_fn(RTLD_NEXT, "_Exit");

        assert(real_pthread_mutex_init);
        assert(real_pthread_mutex_destroy);
        assert(real_pthread_mutex_lock);
        assert(real_pthread_mutex_trylock);
        assert(real_pthread_mutex_timedlock);
        assert(real_pthread_mutex_unlock);
        assert(real_pthread_cond_wait);
        assert(real_pthread_cond_timedwait);
        assert(real_exit);
        assert(real__Exit);

        if ((e = getenv("MUTRACE_HASH_SIZE"))) {
                char *x = NULL;

                errno = 0;
                hash_size = strtoul(e, &x, 0);

                if (!x || *x || errno != 0 || hash_size <= 0)
                        fprintf(stderr,
                                "\n"
                                "mutrace: WARNING: Failed to parse MUTRACE_HASH_SIZE (%s).\n", e);
        }

        if ((e = getenv("MUTRACE_FRAMES"))) {
                char *x = NULL;

                errno = 0;
                frames_max = strtoul(e, &x, 0);

                if (!x || *x || errno != 0 || frames_max <= 0)
                        fprintf(stderr,
                                "\n"
                                "mutrace: WARNING: Failed to parse MUTRACE_FRAMES (%s).\n", e);
        }

        alive_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(alive_mutexes);

        dead_mutexes = calloc(hash_size, sizeof(struct mutex_info*));
        assert(dead_mutexes);

        mutexes_lock = malloc(hash_size * sizeof(pthread_mutex_t));
        assert(mutexes_lock);

        for (m = mutexes_lock, last = mutexes_lock+hash_size; m < last; m++) {
                r = real_pthread_mutex_init(m, NULL);

                assert(r == 0);
        }

        fprintf(stderr, "mutrace: "PACKAGE_VERSION" sucessfully initialized.\n");
}

static unsigned long mutex_hash(pthread_mutex_t *mutex) {
        unsigned long u;

        u = (unsigned long) mutex;
        do {
                u = (u % hash_size) ^ (u / hash_size);
        } while (u > hash_size);

        return u;
}

static void lock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_trylock(mutexes_lock + u);

        if (r == EBUSY) {
                __sync_fetch_and_add(&n_self_contended, 1);
                r = real_pthread_mutex_lock(mutexes_lock + u);
        }

        assert(r == 0);
}

static void unlock_hash_mutex(unsigned u) {
        int r;

        r = real_pthread_mutex_unlock(mutexes_lock + u);
        assert(r == 0);
}

static int mutex_info_compare(const void *_a, const void *_b) {
        const struct mutex_info
                *a = *(const struct mutex_info**) _a,
                *b = *(const struct mutex_info**) _b;

        if (a->n_contended > b->n_contended)
                return -1;
        else if (a->n_contended < b->n_contended)
                return 1;

        if (a->n_owner_changed > b->n_owner_changed)
                return -1;
        else if (a->n_owner_changed < b->n_owner_changed)
                return 1;

        if (a->n_locked > b->n_locked)
                return -1;
        else if (a->n_locked < b->n_locked)
                return 1;

        if (a->nsec_locked_max > b->nsec_locked_max)
                return -1;
        else if (a->nsec_locked_max < b->nsec_locked_max)
                return 1;

        /* Let's make the output deterministic */
        if (a > b)
                return -1;
        else if (a < b)
                return 1;

        return 0;
}

static bool mutex_info_show(struct mutex_info *mi) {

        if (mi->n_locked < show_n_locked_min)
                return false;

        if (mi->n_owner_changed < show_n_owner_changed_min)
                return false;

        if (mi->n_contended < show_n_contended_min)
                return false;

        return true;
}

static void mutex_info_dump(struct mutex_info *mi) {

        if (!mutex_info_show(mi))
                return;

        fprintf(stderr,
                "\nMutex #%u (0x%p) first referenced by:\n"
                "%s", mi->id, mi->mutex, mi->stacktrace);
}

static void mutex_info_stat(struct mutex_info *mi) {

        if (!mutex_info_show(mi))
                return;

        fprintf(stderr,
                "%8u %8u %8u %8u %12.3f %12.3f %12.3f%s\n",
                mi->id,
                mi->n_locked,
                mi->n_owner_changed,
                mi->n_contended,
                (double) mi->nsec_locked_total / 1000000.0,
                (double) mi->nsec_locked_total / mi->n_locked / 1000000.0,
                (double) mi->nsec_locked_max / 1000000.0,
                mi->broken ? " inconsistent!" : "");
}

static void show_summary(void) {
        static pthread_mutex_t summary_mutex = PTHREAD_MUTEX_INITIALIZER;
        static bool shown_summary = false;

        struct mutex_info *mi, **table;
        unsigned n, u, i;


        real_pthread_mutex_lock(&summary_mutex);

        if (shown_summary)
                goto finish;

        if (n_broken > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u inconsistent mutex uses detected. Results might not be reliable.\n"
                        "mutrace:          Fix your program first!\n", n_broken);

        if (n_collisions > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal hash collisions detected. Results might not be as reliable as they could be.\n"
                        "mutrace:          Try to increase MUTRACE_HASH_SIZE, which is currently at %lu.\n", n_collisions, hash_size);

        if (n_self_contended > 0)
                fprintf(stderr,
                        "\n"
                        "mutrace: WARNING: %u internal mutex contention detected. Results might not be reliable as they could be.\n"
                        "mutrace:          Try to increase MUTRACE_HASH_SIZE, which is currently at %lu.\n", n_self_contended, hash_size);

        n = 0;
        for (u = 0; u < hash_size; u++) {
                lock_hash_mutex(u);

                for (mi = alive_mutexes[u]; mi; mi = mi->next)
                        n++;

                for (mi = dead_mutexes[u]; mi; mi = mi->next)
                        n++;
        }

        if (n <= 0) {
                fprintf(stderr,
                        "\n"
                        "mutrace: No mutexes used.\n");
                goto finish;
        }

        fprintf(stderr,
                "\n"
                "mutrace: %u mutexes used.\n", n);

        table = malloc(sizeof(struct mutex_info*) * n);

        i = 0;
        for (u = 0; u < hash_size; u++) {
                for (mi = alive_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }

                for (mi = dead_mutexes[u]; mi; mi = mi->next) {
                        mi->id = i;
                        table[i++] = mi;
                }
        }
        assert(i == n);

        for (i = 0; i < n; i++)
                mutex_info_dump(table[i]);

        qsort(table, n, sizeof(table[0]), mutex_info_compare);

        fprintf(stderr,
                "\n Mutex #   Locked  Changed    Cont. tot.Time[ms] avg.Time[ms] max.Time[ms]\n");

        for (i = 0; i < n; i++)
                mutex_info_stat(table[i]);

        free(table);

        for (u = 0; u < hash_size; u++)
                unlock_hash_mutex(u);

finish:
        shown_summary = true;

        real_pthread_mutex_unlock(&summary_mutex);
}

static void shutdown(void) {
        show_summary();
}

void exit(int status) {
        show_summary();
        real_exit(status);
}

void _Exit(int status) {
        show_summary();
        real__Exit(status);
}

static bool verify_frame(const char *s) {

        if (strstr(s, "/" SONAME "("))
                return false;

        return true;
}

static char* generate_stacktrace(void) {
        void **buffer;
        char **strings, *ret, *p;
        int n, i;
        size_t k;
        bool b;

        buffer = malloc(sizeof(void*) * frames_max);
        assert(buffer);

        n = backtrace(buffer, frames_max);
        assert(n >= 0);

        strings = backtrace_symbols(buffer, n);
        assert(strings);

        free(buffer);

        k = 0;
        for (i = 0; i < n; i++)
                k += strlen(strings[i]) + 2;

        ret = malloc(k + 1);
        assert(ret);

        b = false;
        for (i = 0, p = ret; i < n; i++) {
                if (!b && !verify_frame(strings[i]))
                        continue;

                if (!b && i > 0) {
                        /* Skip all but the first stack frame of ours */
                        *(p++) = '\t';
                        strcpy(p, strings[i-1]);
                        p += strlen(strings[i-1]);
                        *(p++) = '\n';
                }

                b = true;

                *(p++) = '\t';
                strcpy(p, strings[i]);
                p += strlen(strings[i]);
                *(p++) = '\n';
        }

        *p = 0;

        free(strings);

        return ret;
}

static struct mutex_info *mutex_info_add(unsigned long u, pthread_mutex_t *mutex) {
        struct mutex_info *mi;

        /* Needs external locking */

        if (alive_mutexes[u])
                __sync_fetch_and_add(&n_collisions, 1);

        mi = calloc(1, sizeof(struct mutex_info));
        assert(mi);

        mi->mutex = mutex;
        mi->stacktrace = generate_stacktrace();

        mi->next = alive_mutexes[u];
        alive_mutexes[u] = mi;

        return mi;
}

static void mutex_info_remove(unsigned u, pthread_mutex_t *mutex) {
        struct mutex_info *mi, *p;

        /* Needs external locking */

        for (mi = alive_mutexes[u], p = NULL; mi; p = mi, mi = mi->next)
                if (mi->mutex == mutex)
                        break;

        if (!mi)
                return;

        if (p)
                p->next = mi->next;
        else
                alive_mutexes[u] = mi->next;

        mi->next = dead_mutexes[u];
        dead_mutexes[u] = mi;
}

static struct mutex_info *mutex_info_acquire(pthread_mutex_t *mutex) {
        unsigned long u;
        struct mutex_info *mi;

        u = mutex_hash(mutex);
        lock_hash_mutex(u);

        for (mi = alive_mutexes[u]; mi; mi = mi->next)
                if (mi->mutex == mutex)
                        return mi;

        return mutex_info_add(u, mutex);
}

static void mutex_info_release(pthread_mutex_t *mutex) {
        unsigned long u;

        u = mutex_hash(mutex);
        unlock_hash_mutex(u);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr) {
        int r;
        unsigned long u;

        r = real_pthread_mutex_init(mutex, mutexattr);
        if (r != 0)
                return r;

        if (!recursive) {
                recursive = true;
                u = mutex_hash(mutex);
                lock_hash_mutex(u);

                mutex_info_remove(u, mutex);
                mutex_info_add(u, mutex);

                unlock_hash_mutex(u);
                recursive = false;
        }

        return r;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
        unsigned long u;

        u = mutex_hash(mutex);
        lock_hash_mutex(u);

        mutex_info_remove(u, mutex);

        unlock_hash_mutex(u);

        return real_pthread_mutex_destroy(mutex);
}

static void mutex_lock(pthread_mutex_t *mutex, bool busy) {
        int r;
        struct mutex_info *mi;
        pid_t tid;

        if (recursive)
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level > 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;
        }

        mi->n_lock_level++;
        mi->n_locked++;

        if (busy)
                mi->n_contended++;

        tid = _gettid();
        if (mi->last_owner != tid) {
                if (mi->last_owner != 0)
                        mi->n_owner_changed++;

                mi->last_owner = tid;
        }

        r = clock_gettime(CLOCK_MONOTONIC, &mi->timestamp);
        assert(r == 0);

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
        int r;
        bool busy;

        r = real_pthread_mutex_trylock(mutex);
        if (r != EBUSY && r != 0)
                return r;

        if ((busy = (r == EBUSY))) {
                r = real_pthread_mutex_lock(mutex);

                if (r != 0)
                        return r;
        }

        mutex_lock(mutex, busy);
        return r;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
        int r;
        bool busy;

        r = real_pthread_mutex_trylock(mutex);
        if (r != EBUSY && r != 0)
                return r;

        if ((busy = (r == EBUSY))) {
                r = real_pthread_mutex_timedlock(mutex, abstime);

                if (r == ETIMEDOUT)
                        busy = true;
                else if (r != 0)
                        return r;
        }

        mutex_lock(mutex, busy);
        return r;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
        int r;

        r = real_pthread_mutex_trylock(mutex);
        if (r != EBUSY && r != 0)
                return r;

        mutex_lock(mutex, r == EBUSY);
        return r;
}

static void mutex_unlock(pthread_mutex_t *mutex) {
        struct mutex_info *mi;
        struct timespec now;
        uint64_t t;
        int r;

        if (recursive)
                return;

        recursive = true;
        mi = mutex_info_acquire(mutex);

        if (mi->n_lock_level <= 0) {
                __sync_fetch_and_add(&n_broken, 1);
                mi->broken = true;
        }

        mi->n_lock_level--;

        r = clock_gettime(CLOCK_MONOTONIC, &now);
        assert(r == 0);

        t =
                (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec -
                (uint64_t) mi->timestamp.tv_sec * 1000000000ULL - (uint64_t) mi->timestamp.tv_nsec;

        mi->nsec_locked_total += t;

        if (t > mi->nsec_locked_max)
                mi->nsec_locked_max = t;

        mutex_info_release(mutex);
        recursive = false;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {

        mutex_unlock(mutex);

        return real_pthread_mutex_unlock(mutex);
}

/* int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) { */
/*         int r; */

/*         mutex_unlock(mutex); */
/*         r = real_pthread_cond_wait(cond, mutex); */

/*         /\* Unfortunately we cannot distuingish mutex contention and */
/*          * the condition not being signalled here. *\/ */
/*         mutex_lock(mutex, false); */

/*         return r; */
/* } */

/* int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) { */
/*         int r; */

/*         mutex_unlock(mutex); */
/*         r = real_pthread_cond_timedwait(cond, mutex, abstime); */
/*         mutex_lock(mutex, false); */

/*         return r; */
/* } */
