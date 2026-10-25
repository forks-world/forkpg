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
static int (*real_rename)(const char *, const char *);
static int metadata_renamed;
static int selector_renamed;
static int (*real_renameat)(int, const char *, int, const char *);
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
	real_rename = dlsym(RTLD_NEXT, "rename");
	real_renameat = dlsym(RTLD_NEXT, "renameat");
	root = getenv("PAGESTORE_IO_TEST_ROOT");
	arm = getenv("PAGESTORE_IO_TEST_ARM");
	report = getenv("PAGESTORE_IO_TEST_REPORT");
	operation = getenv("PAGESTORE_IO_TEST_OPERATION");
	if (!real_pwrite || !real_pwrite64 || !real_fsync || !real_rename || !real_renameat || !root || !arm || !report ||
		!operation || root[0] != '/' || arm[0] != '/' || report[0] != '/' ||
		strlen(root) >= PATH_MAX || under_root(arm) || under_root(report) ||
		(strcmp(operation, "pwrite") != 0 && strcmp(operation, "fsync") != 0 &&
		 strcmp(operation, "short-pwrite") != 0 &&
		 strcmp(operation, "wal-fsync") != 0 &&
		 strcmp(operation, "forkmeta-fsync") != 0 &&
		 strcmp(operation, "metadata-dir-fsync") != 0 &&
		 strcmp(operation, "selector-dir-fsync") != 0))
		_exit(125);
}

static int
inject(int fd, const char *op, const void *buffer, size_t count,
	   off64_t offset, ssize_t *result)
{
	char proc[64], path[PATH_MAX], line[256];
	ssize_t length;
	int saved = errno;
	int error = strcmp(op, "pwrite") == 0 ? ENOSPC : EIO;
	int output;
	int selector_sync = strcmp(operation, "selector-dir-fsync") == 0;
	int directory_sync = strcmp(operation, "metadata-dir-fsync") == 0;
	int forkmeta_sync = strcmp(operation, "forkmeta-fsync") == 0;
	int wal_sync = strcmp(operation, "wal-fsync") == 0;
	int short_write = strcmp(operation, "short-pwrite") == 0;

	if ((strcmp(operation, op) != 0 && !(short_write && strcmp(op, "pwrite") == 0) &&
		 !((wal_sync || forkmeta_sync || directory_sync || selector_sync) && strcmp(op, "fsync") == 0)) ||
		access(arm, F_OK) != 0)
		goto inactive;
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	length = readlink(proc, path, sizeof(path) - 1);
	if (length < 0 || (size_t) length >= sizeof(path) - 1)
		goto inactive;
	path[length] = '\0';
	if (!under_root(path))
		goto inactive;
	if (selector_sync)
	{
		if (strcmp(path + strlen(root), "/forkmeta_snapshots") != 0 ||
			!__atomic_load_n(&selector_renamed, __ATOMIC_ACQUIRE))
			goto inactive;
	}
	else if (directory_sync)
	{
		if (strcmp(path, root) != 0 || !__atomic_load_n(&metadata_renamed, __ATOMIC_ACQUIRE))
			goto inactive;
	}
	else if (forkmeta_sync)
	{
		if (strcmp(path + strlen(root), "/forkmeta") != 0)
			goto inactive;
	}
	else if (wal_sync)
	{
		const char *name = path + strlen(root);
		if (strncmp(name, "/wal_", 5) != 0 || name[5] == '\0' ||
			strspn(name + 5, "0123456789") != strlen(name + 5))
			goto inactive;
	}
	else if (strncmp(path + strlen(root), "/seg_", 5) != 0)
		goto inactive;
	/* Leave small record headers intact so SQL probes hit page payloads. */
	if (short_write && count < 512)
		goto inactive;
	*result = -1;
	if (short_write && count > 1)
	{
		/* Mutate real segment bytes; never fabricate a successful byte count.
		 * Retried writes remain short while the fault is armed. */
		*result = real_pwrite64(fd, buffer, count / 2, offset);
		error = *result < 0 ? errno : 0;
	}
	output = open(report, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
	if (output >= 0)
	{
		int bytes = snprintf(line, sizeof(line),
			"{\"operation\":\"%s\",\"errno\":%d,\"pid\":%ld,"
			"\"requested\":%zu,\"written\":%ld}\n", operation, error, (long) getpid(),
			count, (long) *result);
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
	ssize_t result;
	return inject(fd, "pwrite", buffer, length, offset, &result) ? result :
		real_pwrite(fd, buffer, length, offset);
}

ssize_t
pwrite64(int fd, const void *buffer, size_t length, off64_t offset)
{
	ssize_t result;
	return inject(fd, "pwrite", buffer, length, offset, &result) ? result :
		real_pwrite64(fd, buffer, length, offset);
}

int
fsync(int fd)
{
	ssize_t result;
	return inject(fd, "fsync", NULL, 0, 0, &result) ? -1 : real_fsync(fd);
}

/* Only a successful publication of the exact forkmeta replacement arms the
 * directory boundary. Unrelated renames and ordinary directory syncs pass. */
int
rename(const char *source, const char *destination)
{
	int result = real_rename(source, destination);
	int saved = errno;
	if (result == 0 && strcmp(operation, "metadata-dir-fsync") == 0 &&
		access(arm, F_OK) == 0 && under_root(source) && under_root(destination) &&
		strcmp(source + strlen(root), "/forkmeta.rewrite.tmp") == 0 &&
		strcmp(destination + strlen(root), "/forkmeta") == 0)
		__atomic_store_n(&metadata_renamed, 1, __ATOMIC_RELEASE);
	errno = saved;
	return result;
}

static int
snapshot_directory(int fd)
{
	char proc[64], path[PATH_MAX];
	ssize_t length;
	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	length = readlink(proc, path, sizeof(path) - 1);
	if (length < 0 || (size_t) length >= sizeof(path) - 1)
		return 0;
	path[length] = '\0';
	return under_root(path) && strcmp(path + strlen(root), "/forkmeta_snapshots") == 0;
}

int
renameat(int source_fd, const char *source, int destination_fd, const char *destination)
{
	int result = real_renameat(source_fd, source, destination_fd, destination);
	int saved = errno;
	const char *prefix = "forkmeta_manifest_v1.tmp.";
	if (result == 0 && strcmp(operation, "selector-dir-fsync") == 0 &&
		access(arm, F_OK) == 0 && snapshot_directory(source_fd) &&
		snapshot_directory(destination_fd) && strncmp(source, prefix, strlen(prefix)) == 0 &&
		strcmp(destination, "forkmeta_manifest_v1") == 0)
		__atomic_store_n(&selector_renamed, 1, __ATOMIC_RELEASE);
	errno = saved;
	return result;
}
