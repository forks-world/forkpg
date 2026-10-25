/* Test-only Linux preload: inject page-segment I/O errors in one daemon. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Export both libc entry points even when the project uses large-file aliases. */
#undef _FILE_OFFSET_BITS
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off64_t);
static int (*real_fsync)(int);
static const char *root, *arm, *report, *operation;

static int
under_root(const char *path)
{
    size_t len = strlen(root);
    return strncmp(path, root, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

__attribute__((constructor)) static void
initialize(void)
{
    real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    root = getenv("PAGESTORE_IO_TEST_ROOT");
    arm = getenv("PAGESTORE_IO_TEST_ARM");
    report = getenv("PAGESTORE_IO_TEST_REPORT");
    operation = getenv("PAGESTORE_IO_TEST_OPERATION");
    if (!real_pwrite || !real_pwrite64 || !real_fsync || !root || !arm || !report ||
        !operation || root[0] != '/' || arm[0] != '/' || report[0] != '/' ||
        strlen(root) >= PATH_MAX || under_root(arm) || under_root(report) ||
        (strcmp(operation, "pwrite") != 0 && strcmp(operation, "fsync") != 0))
        _exit(125);
}

static int
inject(int fd, const char *op)
{
    char proc[64], path[PATH_MAX], line[160];
    ssize_t length;
    int saved = errno;
    int error = strcmp(op, "pwrite") == 0 ? ENOSPC : EIO;
    int output;

    if (strcmp(operation, op) != 0 || access(arm, F_OK) != 0)
        goto inactive;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    length = readlink(proc, path, sizeof(path) - 1);
    if (length < 0 || (size_t) length >= sizeof(path) - 1)
        goto inactive;
    path[length] = '\0';
    if (!under_root(path) || strncmp(path + strlen(root), "/seg_", 5) != 0)
        goto inactive;
    output = open(report, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (output >= 0)
    {
        int bytes = snprintf(line, sizeof(line),
            "{\"operation\":\"%s\",\"errno\":%d,\"pid\":%ld}\n", op, error, (long) getpid());
        (void) syscall(SYS_write, output, line, (size_t) bytes);
        (void) real_fsync(output);
        (void) close(output);
    }
    errno = error;
    return 1;
inactive:
    errno = saved;
    return 0;
}

ssize_t
pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
    return inject(fd, "pwrite") ? -1 : real_pwrite(fd, buffer, length, offset);
}

ssize_t
pwrite64(int fd, const void *buffer, size_t length, off64_t offset)
{
    return inject(fd, "pwrite") ? -1 : real_pwrite64(fd, buffer, length, offset);
}

int
fsync(int fd)
{
    return inject(fd, "fsync") ? -1 : real_fsync(fd);
}
