/*-------------------------------------------------------------------------
 *
 * fuzz_standalone_driver.c
 *	  Non-instrumented replay driver: feeds a directory of corpus files
 *	  through ps_fuzz_run_one() (the same LLVMFuzzerTestOneInput path the
 *	  libFuzzer binary uses) without linking libFuzzer, ASan or UBSan.
 *
 * This is what the meson test suite runs (pagestore_format_fuzz_replay,
 * see meson.build): a corpus regression pass that works with the project's
 * ordinary compiler, so a clang+libFuzzer toolchain is not a CI
 * requirement.  It only catches hard crashes (SIGSEGV/SIGABRT, including
 * PAGESTORE_ASSERT_CHECKING assertions in a cassert build); it does not
 * catch the ASan/UBSan findings the instrumented binary does, which is why
 * it is a regression check on known-good corpus files, not a fuzz run.
 *
 * Usage: pagestore_format_fuzz_replay <target-name> <corpus-dir> [<corpus-dir> ...]
 * <target-name> is "all" or one of the names in fuzz_common.c's
 * ps_fuzz_targets table.  Each <corpus-dir> is scanned non-recursively for
 * regular files; a directory or file named "known-crashes" is skipped, since
 * those are confirmed findings replayed separately, not a regression gate
 * that would block CI.
 *
 *-------------------------------------------------------------------------
 */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fuzz_common.h"

static long total_files = 0;
static int replay_failed = 0;

static void
replay_error(const char *operation, const char *path)
{
	fprintf(stderr, "%s: %s: %s\n", operation, path, strerror(errno));
	replay_failed = 1;
}

static void
replay_file(const char *target, const char *path)
{
	FILE	   *f = fopen(path, "rb");
	long		len;
	uint8_t    *buf;

	if (f == NULL)
	{
		replay_error("fopen", path);
		return;
	}
	if (fseek(f, 0, SEEK_END) != 0)
	{
		replay_error("fseek", path);
		fclose(f);
		return;
	}
	len = ftell(f);
	if (len < 0)
	{
		replay_error("ftell", path);
		fclose(f);
		return;
	}
	if (fseek(f, 0, SEEK_SET) != 0)
	{
		replay_error("fseek", path);
		fclose(f);
		return;
	}
	buf = malloc((size_t) len > 0 ? (size_t) len : 1);
	if (buf == NULL)
	{
		errno = ENOMEM;
		replay_error("malloc", path);
		fclose(f);
		return;
	}
	if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len)
	{
		if (ferror(f))
			replay_error("fread", path);
		else
		{
			errno = EIO;
			replay_error("short read", path);
		}
		free(buf);
		fclose(f);
		return;
	}
	if (fclose(f) != 0)
	{
		replay_error("fclose", path);
		free(buf);
		return;
	}

	fprintf(stderr, "replay: %s (%ld bytes)\n", path, len);
	ps_fuzz_run_one(target, buf, (size_t) len);
	free(buf);
	total_files++;
}

static void
replay_dir(const char *target, const char *dirpath)
{
	DIR		   *d = opendir(dirpath);
	struct dirent *ent;
	long		files_before = total_files;

	if (d == NULL)
	{
		replay_error("opendir", dirpath);
		return;
	}
	for (;;)
	{
		char		path[4096];
		struct stat st;
		int			pathlen;

		errno = 0;
		ent = readdir(d);
		if (ent == NULL)
		{
			if (errno != 0)
				replay_error("readdir", dirpath);
			break;
		}
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (strcmp(ent->d_name, "known-crashes") == 0)
			continue;
		pathlen = snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
		if (pathlen < 0 || pathlen >= (int) sizeof(path))
		{
			errno = ENAMETOOLONG;
			replay_error("path too long", dirpath);
			continue;
		}
		if (stat(path, &st) != 0)
		{
			replay_error("stat", path);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		replay_file(target, path);
	}
	if (closedir(d) != 0)
		replay_error("closedir", dirpath);
	if (total_files == files_before)
	{
		fprintf(stderr, "no corpus files: %s\n", dirpath);
		replay_failed = 1;
	}
}

int
main(int argc, char **argv)
{
	const char *target;
	int			i;

	if (argc < 3)
	{
		fprintf(stderr,
				"usage: %s <target-name|all> <corpus-dir> [<corpus-dir> ...]\n",
				argv[0]);
		return 2;
	}
	target = argv[1];

	/* Codex finding on PR #303: validate the target once, before touching
	 * any corpus directory, rather than letting a bad target argument
	 * silently turn the whole replay into a no-op that still reports
	 * success (see ps_fuzz_target_is_valid()'s comment in fuzz_common.h). */
	if (!ps_fuzz_target_is_valid(target))
		return 2;

	ps_fuzz_global_init();
	for (i = 2; i < argc; i++)
		replay_dir(target, argv[i]);

	fprintf(stderr, "replayed %ld corpus file(s) for target '%s'\n",
			total_files, target);
	return replay_failed ? 1 : 0;
}
