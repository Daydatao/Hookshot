/* The host's log writer (host/hos_thread.c, the Windows queue, log_main and log_flush) extracted unmodified, with the CRT
   stream calls as spies: what goes out, in how many writes, and when. Real threads, SRW locks and condition variables. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef int BOOL;
#define WINAPI
#define INFINITE 0xFFFFFFFFu
#define STD_ERROR_HANDLE ((DWORD)-12)
#define EXCEPTION_CONTINUE_SEARCH 0
typedef struct { int unused; } EXCEPTION_POINTERS;
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
static LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) { return f; }
static HANDLE GetStdHandle(DWORD w) { (void)w; return (HANDLE)(intptr_t)2; }
static BOOL WriteFile(HANDLE h, const void *b, DWORD n, DWORD *w, void *o) {
    (void)h, (void)o;
    ssize_t r = write(STDERR_FILENO, b, n);
    if (w) *w = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}
#define _strdup strdup
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(l); }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(l); }
static BOOL TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_mutex_trylock(l) == 0; }
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
static void WakeConditionVariable(CONDITION_VARIABLE *cv) { pthread_cond_signal(cv); }
static ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
}
static BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, DWORD flags) {
    (void)flags;
    if (ms == INFINITE) return pthread_cond_wait(cv, l) == 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    return pthread_cond_timedwait(cv, l, &ts) == 0;
}
static void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
static DWORD GetCurrentThreadId(void) { return (DWORD)(uintptr_t)pthread_self(); }
typedef struct { DWORD (*fn)(void *); void *arg; } PosixThreadStart;
static void *posix_thread_entry(void *p) {
    PosixThreadStart s = *(PosixThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}
static HANDLE CreateThread(void *a, size_t st, DWORD (*fn)(void *), void *arg, DWORD f, DWORD *id) {
    (void)a, (void)st, (void)f, (void)id;
    PosixThreadStart *s = (PosixThreadStart *)malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, posix_thread_entry, s) != 0) { free(s); return NULL; }
    pthread_detach(th);
    return (HANDLE)(uintptr_t)1;
}
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SRWLOCK spy_lock = SRWLOCK_INIT;
static char out[1 << 20];
static size_t out_len;
static volatile LONG writes;

static void spy_append(const void *p, size_t n) {
    AcquireSRWLockExclusive(&spy_lock);
    if (out_len + n <= sizeof out) memcpy(out + out_len, p, n), out_len += n;
    ReleaseSRWLockExclusive(&spy_lock);
    InterlockedIncrement(&writes);
}
__attribute__((unused)) static int spy_fputs(const char *s, FILE *f) {
    (void)f;
    spy_append(s, strlen(s));
    return 0;
}
__attribute__((unused)) static size_t spy_fwrite(const void *p, size_t size, size_t n, FILE *f) {
    (void)f;
    spy_append(p, size * n);
    return n;
}
__attribute__((unused)) static int spy_fflush(FILE *f) {
    (void)f;
    return 0;
}
static size_t out_size(void) {
    AcquireSRWLockExclusive(&spy_lock);
    const size_t n = out_len;
    ReleaseSRWLockExclusive(&spy_lock);
    return n;
}

#define fputs spy_fputs
#define fwrite spy_fwrite
#define fflush spy_fflush
/* (log_main is a thread that never returns: its for (;;) has no return after it. GCC's C front end warns for a static function
   with no return statement at all, whatever its loop; clang, which builds the game, does not) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
/* ACTUAL_LOG */
#pragma GCC diagnostic pop
#undef fflush
#undef fwrite
#undef fputs

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static char expect[1 << 20];
static size_t expect_len;
static int queue(int i) {
    char line[64];
    const int n = snprintf(line, sizeof line, "[hos test] line %d\n", i);
    memcpy(expect + expect_len, line, (size_t)n);
    expect_len += (size_t)n;
    return log_queue(line);
}
static int wait_for(size_t n, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (out_size() < n && GetTickCount64() < end) Sleep(1);
    return out_size() >= n;
}
static int same(void) {
    return out_size() == expect_len && memcmp(out, expect, expect_len) == 0;
}

static int run(const char *name) {
    if (!strcmp(name, "batch")) {   /* (HOS_LOG_BATCH_MS=1000: the lines of that time in one write) */
        CHECK(queue(0));
        Sleep(20);
        for (int i = 1; i < 50; i++) CHECK(queue(i));
        Sleep(100);
        CHECK(out_size() == 0);   /* (still waiting for more) */
        CHECK(wait_for(expect_len, 4000) && same());
        CHECK(writes == 1);
        Sleep(400);
        CHECK(queue(50));   /* (the next batch: the writer back to waiting for a line) */
        CHECK(wait_for(expect_len, 4000) && same() && writes == 2);
    } else if (!strcmp(name, "default")) {   /* (no setting: a line waits its 250 ms, then goes out) */
        const ULONGLONG t0 = GetTickCount64();
        CHECK(queue(0));
        CHECK(wait_for(expect_len, 3000) && same());
        CHECK(GetTickCount64() - t0 >= 200);   /* (GetTickCount64 steps in 10-16 ms) */
    } else if (!strcmp(name, "off")) {   /* (HOS_LOG_BATCH_MS=0: each wake's lines at once, as before) */
        /* (the fastest of three deliveries: a loaded CI runner stalled one past 200 ms now and then, and the default's
           250 ms batch never delivers that fast, so the fastest still tells the two apart) */
        ULONGLONG best = ~0ULL;
        for (int i = 0; i < 3; i++) {
            const ULONGLONG t0 = GetTickCount64();
            CHECK(queue(i));
            CHECK(wait_for(expect_len, 3000) && same());
            const ULONGLONG took = GetTickCount64() - t0;
            if (took < best) best = took;
        }
        CHECK(best < 200);
    } else if (!strcmp(name, "burst")) {   /* (HOS_LOG_BATCH_MS=2000: a queue half full goes out without the wait) */
        size_t half = 0;
        for (int i = 0; i < 1500; i++) {
            CHECK(queue(i));   /* (every line queued: none written by this thread) */
            if (i == 1023) half = expect_len;
        }
        CHECK(wait_for(half, 1000));
        CHECK(wait_for(expect_len, 4000) && same());
    } else if (!strcmp(name, "flush")) {   /* (HOS_LOG_BATCH_MS=2000: a way out writes what waits, once) */
        for (int i = 0; i < 10; i++) CHECK(queue(i));
        Sleep(50);
        CHECK(out_size() == 0);
        log_flush();   /* (as atexit, hos_fatal and the window's close do) */
        CHECK(same());
        Sleep(2500);   /* (the writer's wait ends with nothing left to write) */
        CHECK(same());
    } else {
        CHECK(0);
    }
    printf("PASS: %s\n", name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    return run(argv[1]);
}
