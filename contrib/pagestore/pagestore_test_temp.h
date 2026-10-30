/* Helpers for test-owned fixture directories, not production storage paths. */
#ifndef PAGESTORE_TEST_TEMP_H
#define PAGESTORE_TEST_TEMP_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Leave room for derived paths in the test binaries' existing buffers. */
#define PS_TEST_TEMP_PATH_MAX 192

static char *
ps_test_mkdtemp(char *path, size_t capacity)
{
	const char *root = getenv("TMPDIR");
	char name[128];
	char *absolute;
	int n;

	if (root == NULL || *root == '\0')
		root = "/tmp";
	if (strlen(path) >= sizeof(name))
	{
		fprintf(stderr, "temporary fixture name is too long\n");
		exit(2);
	}
	strcpy(name, path);
	absolute = realpath(root, NULL);
	if (absolute == NULL)
	{
		perror("resolve TMPDIR");
		exit(2);
	}
	n = snprintf(path, capacity, "%s/%s", absolute, name);
	free(absolute);
	if (n < 0 || (size_t) n >= capacity)
	{
		fprintf(stderr, "temporary fixture path is too long\n");
		exit(2);
	}
	if (mkdtemp(path) == NULL)
	{
		perror("create temporary fixture directory");
		exit(2);
	}
	return path;
}

/* Never follow a symlink, including one passed as the root of a subtree. */
static void
ps_test_remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *entry;

	if (lstat(path, &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode))
	{
		if (unlink(path) != 0)
			fprintf(stderr, "note: could not remove %s\n", path);
		return;
	}
	dir = opendir(path);
	if (dir == NULL)
	{
		fprintf(stderr, "note: could not open %s for cleanup\n", path);
		return;
	}
	while ((entry = readdir(dir)) != NULL)
	{
		char child[2048];
		int n;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (n < 0 || (size_t) n >= sizeof(child))
		{
			fprintf(stderr, "note: cleanup path is too long under %s\n", path);
			continue;
		}
		ps_test_remove_tree(child);
	}
	closedir(dir);
	if (rmdir(path) != 0)
		fprintf(stderr, "note: could not remove %s\n", path);
}

#endif
