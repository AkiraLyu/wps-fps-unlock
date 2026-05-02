#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*start_fn)(void *);
typedef long long (*elapsed_fn)(const void *);

static void *resolve_symbol(void *handle, const char *name)
{
    dlerror();
    void *sym = dlvsym(handle, name, "Qt_5");
    if (sym)
        return sym;

    dlerror();
    return dlsym(handle, name);
}

int main(void)
{
    const char *qtcore = getenv("WPS_QTCORE");
    if (!qtcore || !*qtcore)
        qtcore = "/usr/lib/office6/libQt5CoreKso.so.5.12.12";

    void *handle = dlopen(qtcore, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "QtCore was not loaded at startup: %s\n", dlerror());
        return 1;
    }

    start_fn start = (start_fn)resolve_symbol(handle, "_ZN6kso_qt13QElapsedTimer5startEv");
    elapsed_fn elapsed_direct = (elapsed_fn)resolve_symbol(handle, "_ZNK6kso_qt13QElapsedTimer7elapsedEv");
    if (!start || !elapsed_direct) {
        fprintf(stderr, "failed to resolve direct QElapsedTimer symbols\n");
        return 1;
    }

    unsigned char timer[64];
    memset(timer, 0, sizeof(timer));

    start(timer);
    usleep(120000);
    printf("startup_direct_ms=%lld\n", elapsed_direct(timer));
    return 0;
}
