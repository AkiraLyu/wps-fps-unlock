#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef long long (*elapsed_fn)(const void *);
typedef long long (*restart_fn)(void *);
typedef long long (*nsecs_elapsed_fn)(const void *);

long long kso_qelapsed_elapsed(const void *self) __asm__("_ZNK6kso_qt13QElapsedTimer7elapsedEv");
long long kso_qelapsed_restart(void *self) __asm__("_ZN6kso_qt13QElapsedTimer7restartEv");
long long kso_qelapsed_nsecs_elapsed(const void *self) __asm__("_ZNK6kso_qt13QElapsedTimer12nsecsElapsedEv");

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_hook_lock = PTHREAD_MUTEX_INITIALIZER;

static double g_scale = 0.5;
static int g_debug = 0;
static int g_debug_limit = 24;
static int g_inline = 1;

static elapsed_fn g_kso_elapsed = NULL;
static restart_fn g_kso_restart = NULL;
static nsecs_elapsed_fn g_kso_nsecs_elapsed = NULL;
static const char *g_kso_elapsed_symbol = "_ZNK6kso_qt13QElapsedTimer7elapsedEv";
static const char *g_kso_restart_symbol = "_ZN6kso_qt13QElapsedTimer7restartEv";
static const char *g_kso_nsecs_elapsed_symbol = "_ZNK6kso_qt13QElapsedTimer12nsecsElapsedEv";
static int g_kso_elapsed_patched = 0;
static int g_kso_restart_patched = 0;
static int g_kso_nsecs_elapsed_patched = 0;

static elapsed_fn g_qt_elapsed = NULL;
static restart_fn g_qt_restart = NULL;
static nsecs_elapsed_fn g_qt_nsecs_elapsed = NULL;

static const unsigned char k_elapsed_prefix[] = {
    0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xfb,
    0x48, 0x89, 0xe6, 0xbf, 0x01, 0x00, 0x00, 0x00,
};

static const unsigned char k_restart_prefix[] = {
    0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec,
    0x10, 0x48, 0x89, 0xfb, 0x4c, 0x8b, 0x37,
};

static void ensure_hooks_ready(void);

static void *resolve_qt5_symbol(const char *name)
{
    dlerror();
    void *sym = dlvsym(RTLD_NEXT, name, "Qt_5");
    if (sym)
        return sym;

    dlerror();
    return dlsym(RTLD_NEXT, name);
}

static double parse_positive_double(const char *value, double fallback)
{
    if (!value || !*value)
        return fallback;

    char *end = NULL;
    double parsed = strtod(value, &end);
    if (end == value || parsed <= 0.0)
        return fallback;

    return parsed;
}

static int parse_positive_int(const char *value, int fallback)
{
    if (!value || !*value)
        return fallback;

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > INT_MAX)
        return fallback;

    return (int)parsed;
}

static int parse_bool_int(const char *value, int fallback)
{
    if (!value || !*value)
        return fallback;

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value)
        return fallback;

    return parsed != 0;
}

static void init_hook(void)
{
    g_scale = parse_positive_double(getenv("WPS_QELAPSED_SCALE"), 0.5);
    g_debug = parse_positive_int(getenv("WPS_QELAPSED_DEBUG"), 0);
    g_debug_limit = parse_positive_int(getenv("WPS_QELAPSED_DEBUG_LIMIT"), 24);
    g_inline = parse_bool_int(getenv("WPS_QELAPSED_INLINE"), 1);

    const char *elapsed_symbol = getenv("WPS_QELAPSED_SYMBOL_ELAPSED");
    const char *restart_symbol = getenv("WPS_QELAPSED_SYMBOL_RESTART");
    const char *nsecs_elapsed_symbol = getenv("WPS_QELAPSED_SYMBOL_NSECS_ELAPSED");
    if (elapsed_symbol && *elapsed_symbol)
        g_kso_elapsed_symbol = elapsed_symbol;
    if (restart_symbol && *restart_symbol)
        g_kso_restart_symbol = restart_symbol;
    if (nsecs_elapsed_symbol && *nsecs_elapsed_symbol)
        g_kso_nsecs_elapsed_symbol = nsecs_elapsed_symbol;

    if (g_debug) {
        fprintf(stderr, "wps-qelapsed-scale: scale=%g inline=%d\n", g_scale, g_inline);
        fprintf(stderr, "wps-qelapsed-scale: elapsed symbol=%s\n", g_kso_elapsed_symbol);
        fprintf(stderr, "wps-qelapsed-scale: restart symbol=%s\n", g_kso_restart_symbol);
        fprintf(stderr, "wps-qelapsed-scale: nsecsElapsed symbol=%s\n", g_kso_nsecs_elapsed_symbol);
    }
}

static void write_abs_jump(unsigned char *where, const void *to, size_t patch_len)
{
    uintptr_t addr = (uintptr_t)to;
    where[0] = 0x48;
    where[1] = 0xb8;
    memcpy(where + 2, &addr, sizeof(addr));
    where[10] = 0xff;
    where[11] = 0xe0;

    for (size_t i = 12; i < patch_len; ++i)
        where[i] = 0x90;
}

static int set_page_protection(void *addr, size_t len, int prot)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return -1;

    uintptr_t start = (uintptr_t)addr & ~((uintptr_t)page_size - 1);
    uintptr_t end = ((uintptr_t)addr + len + (uintptr_t)page_size - 1) & ~((uintptr_t)page_size - 1);
    return mprotect((void *)start, end - start, prot);
}

static void *install_inline_hook(const char *name,
                                 void *target,
                                 void *replacement,
                                 const unsigned char *expected,
                                 size_t patch_len)
{
    if (!target || !replacement)
        return NULL;

    if (patch_len < 12)
        return NULL;

    if (memcmp(target, expected, patch_len) != 0) {
        if (g_debug) {
            fprintf(stderr, "wps-qelapsed-scale: skip inline hook for %s: unexpected prologue\n", name);
        }
        return NULL;
    }

    size_t trampoline_len = patch_len + 12;
    unsigned char *trampoline = mmap(NULL,
                                     trampoline_len,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS,
                                     -1,
                                     0);
    if (trampoline == MAP_FAILED) {
        if (g_debug) {
            fprintf(stderr, "wps-qelapsed-scale: mmap trampoline failed for %s: %s\n", name, strerror(errno));
        }
        return NULL;
    }

    memcpy(trampoline, target, patch_len);
    write_abs_jump(trampoline + patch_len, (const unsigned char *)target + patch_len, 12);
    if (mprotect(trampoline, trampoline_len, PROT_READ | PROT_EXEC) != 0 && g_debug) {
        fprintf(stderr, "wps-qelapsed-scale: mprotect trampoline failed for %s: %s\n", name, strerror(errno));
    }

    if (set_page_protection(target, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        if (g_debug) {
            fprintf(stderr, "wps-qelapsed-scale: mprotect target failed for %s: %s\n", name, strerror(errno));
        }
        munmap(trampoline, trampoline_len);
        return NULL;
    }

    write_abs_jump((unsigned char *)target, replacement, patch_len);
    __builtin___clear_cache((char *)target, (char *)target + patch_len);
    if (set_page_protection(target, patch_len, PROT_READ | PROT_EXEC) != 0 && g_debug) {
        fprintf(stderr, "wps-qelapsed-scale: restore target protection failed for %s: %s\n", name, strerror(errno));
    }

    if (g_debug) {
        fprintf(stderr, "wps-qelapsed-scale: inline hook installed for %s\n", name);
    }
    return trampoline;
}

static void ensure_hooks_ready(void)
{
    pthread_once(&g_init_once, init_hook);

    pthread_mutex_lock(&g_hook_lock);

    if (!g_kso_elapsed)
        g_kso_elapsed = (elapsed_fn)resolve_qt5_symbol(g_kso_elapsed_symbol);
    if (!g_kso_restart)
        g_kso_restart = (restart_fn)resolve_qt5_symbol(g_kso_restart_symbol);
    if (!g_kso_nsecs_elapsed)
        g_kso_nsecs_elapsed = (nsecs_elapsed_fn)resolve_qt5_symbol(g_kso_nsecs_elapsed_symbol);

    if (!g_qt_elapsed)
        g_qt_elapsed = (elapsed_fn)resolve_qt5_symbol("_ZNK13QElapsedTimer7elapsedEv");
    if (!g_qt_restart)
        g_qt_restart = (restart_fn)resolve_qt5_symbol("_ZN13QElapsedTimer7restartEv");
    if (!g_qt_nsecs_elapsed)
        g_qt_nsecs_elapsed = (nsecs_elapsed_fn)resolve_qt5_symbol("_ZNK13QElapsedTimer12nsecsElapsedEv");

    if (g_inline) {
        if (!g_kso_elapsed_patched && g_kso_elapsed) {
            void *trampoline = install_inline_hook("kso_qt::QElapsedTimer::elapsed",
                                                   (void *)g_kso_elapsed,
                                                   (void *)kso_qelapsed_elapsed,
                                                   k_elapsed_prefix,
                                                   sizeof(k_elapsed_prefix));
            if (trampoline) {
                g_kso_elapsed = (elapsed_fn)trampoline;
                g_kso_elapsed_patched = 1;
            }
        }

        if (!g_kso_restart_patched && g_kso_restart) {
            void *trampoline = install_inline_hook("kso_qt::QElapsedTimer::restart",
                                                   (void *)g_kso_restart,
                                                   (void *)kso_qelapsed_restart,
                                                   k_restart_prefix,
                                                   sizeof(k_restart_prefix));
            if (trampoline) {
                g_kso_restart = (restart_fn)trampoline;
                g_kso_restart_patched = 1;
            }
        }

        if (!g_kso_nsecs_elapsed_patched && g_kso_nsecs_elapsed) {
            void *trampoline = install_inline_hook("kso_qt::QElapsedTimer::nsecsElapsed",
                                                   (void *)g_kso_nsecs_elapsed,
                                                   (void *)kso_qelapsed_nsecs_elapsed,
                                                   k_elapsed_prefix,
                                                   sizeof(k_elapsed_prefix));
            if (trampoline) {
                g_kso_nsecs_elapsed = (nsecs_elapsed_fn)trampoline;
                g_kso_nsecs_elapsed_patched = 1;
            }
        }
    }

    pthread_mutex_unlock(&g_hook_lock);
}

static void preload_ctor(void) __attribute__((constructor));
static void preload_ctor(void)
{
    ensure_hooks_ready();
}

static long long scale_i64(long long raw)
{
    if (raw <= 0)
        return raw;

    long double scaled = (long double)raw * (long double)g_scale;
    if (scaled >= (long double)LLONG_MAX)
        return LLONG_MAX;

    long long out = (long long)(scaled + 0.5L);
    if (out == 0)
        return 1;

    return out;
}

static void debug_call(const char *name, long long raw, long long scaled)
{
    if (!g_debug)
        return;

    static unsigned int count = 0;
    unsigned int current = __sync_add_and_fetch(&count, 1);
    if ((int)current <= g_debug_limit) {
        fprintf(stderr, "wps-qelapsed-scale: %s raw=%lld scaled=%lld\n", name, raw, scaled);
    }
}

static long long call_elapsed(const char *name, elapsed_fn *slot, const void *self)
{
    ensure_hooks_ready();
    elapsed_fn fn = *slot;
    if (!fn) {
        if (g_debug)
            fprintf(stderr, "wps-qelapsed-scale: missing original symbol for %s\n", name);
        return 0;
    }

    long long raw = fn(self);
    long long scaled = scale_i64(raw);
    debug_call(name, raw, scaled);
    return scaled;
}

static long long call_restart(const char *name, restart_fn *slot, void *self)
{
    ensure_hooks_ready();
    restart_fn fn = *slot;
    if (!fn) {
        if (g_debug)
            fprintf(stderr, "wps-qelapsed-scale: missing original symbol for %s\n", name);
        return 0;
    }

    long long raw = fn(self);
    long long scaled = scale_i64(raw);
    debug_call(name, raw, scaled);
    return scaled;
}

static bool call_has_expired(const char *name, elapsed_fn *slot, const void *self, long long timeout)
{
    if (timeout < 0)
        return false;

    long long elapsed = call_elapsed(name, slot, self);
    return elapsed > timeout;
}

long long kso_qelapsed_elapsed(const void *self)
{
    return call_elapsed("kso_qt::QElapsedTimer::elapsed", &g_kso_elapsed, self);
}

long long kso_qelapsed_restart(void *self)
{
    return call_restart("kso_qt::QElapsedTimer::restart", &g_kso_restart, self);
}

long long kso_qelapsed_nsecs_elapsed(const void *self)
{
    return call_elapsed("kso_qt::QElapsedTimer::nsecsElapsed", &g_kso_nsecs_elapsed, self);
}

bool kso_qelapsed_has_expired(const void *self, long long timeout) __asm__("_ZNK6kso_qt13QElapsedTimer10hasExpiredEx");
bool kso_qelapsed_has_expired(const void *self, long long timeout)
{
    return call_has_expired("kso_qt::QElapsedTimer::hasExpired", &g_kso_elapsed, self, timeout);
}

long long qt_qelapsed_elapsed(const void *self) __asm__("_ZNK13QElapsedTimer7elapsedEv");
long long qt_qelapsed_elapsed(const void *self)
{
    return call_elapsed("QElapsedTimer::elapsed", &g_qt_elapsed, self);
}

long long qt_qelapsed_restart(void *self) __asm__("_ZN13QElapsedTimer7restartEv");
long long qt_qelapsed_restart(void *self)
{
    return call_restart("QElapsedTimer::restart", &g_qt_restart, self);
}

long long qt_qelapsed_nsecs_elapsed(const void *self) __asm__("_ZNK13QElapsedTimer12nsecsElapsedEv");
long long qt_qelapsed_nsecs_elapsed(const void *self)
{
    return call_elapsed("QElapsedTimer::nsecsElapsed", &g_qt_nsecs_elapsed, self);
}

bool qt_qelapsed_has_expired(const void *self, long long timeout) __asm__("_ZNK13QElapsedTimer10hasExpiredEx");
bool qt_qelapsed_has_expired(const void *self, long long timeout)
{
    return call_has_expired("QElapsedTimer::hasExpired", &g_qt_elapsed, self, timeout);
}
