#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_flag held = ATOMIC_FLAG_INIT;

FILE *fopen(const char *path, const char *mode)
{
    FILE *(*original)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
    const char *target = getenv("UX001_HOLD_SOURCE");
    const char *entered = getenv("UX001_ENTERED_FD");
    const char *release = getenv("UX001_RELEASE_FD");
    if (target != NULL && entered != NULL && release != NULL &&
        strcmp(path, target) == 0 && strcmp(mode, "rb") == 0 &&
        !atomic_flag_test_and_set(&held)) {
        char byte = 'x';
        if (write(atoi(entered), &byte, 1) != 1) _exit(91);
        ssize_t result;
        do {
            result = read(atoi(release), &byte, 1);
        } while (result < 0 && errno == EINTR);
        if (result != 1) _exit(92);
    }
    if (original == NULL) _exit(93);
    return original(path, mode);
}
