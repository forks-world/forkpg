/*-------------------------------------------------------------------------
 *
 * gate_sweep.c
 *	  Mechanical pre-parse-gate sweep for every persisted-format fuzz
 *	  target's CRC fixup (fuzz_crc_fixup.c).
 *
 * PR #303 round 7: three rounds of manual, per-target reading of the
 * product loader against fuzz_crc_fixup.c (see fuzz/FIXUP_GATES.md) still
 * missed three real gates that Codex's review found. The manual method
 * does not scale; this replaces judgment with brute force wherever brute
 * force is cheap enough to run.
 *
 * For every target in fuzz_common.c's ps_fuzz_targets[] table: take the
 * pristine template file (the same in-memory bytes fixup_* is written to
 * assume as its "real" cross-file identity elsewhere), then for every byte
 * offset in the first GATE_SWEEP_BYTES bytes of that file, XOR the byte
 * with 0xFF and separately with 0x01, run the real ps_fuzz_crc_fixup() the
 * same way an iteration under PS_FUZZ_CRC_FIXUP=always would, write the
 * result to the live work_dir, and call the real ps_core_open() +
 * ps_core_maintenance() + ps_core_close(). Every ps_core_open() failure
 * path -- product invariant, not something this tool assumes -- prints
 * exactly one "pagestore_core: open step <name> failed: <errno>" line
 * (open_step_failed()/OPEN_STEP() in pagestore_core.c; see that function's
 * own comment: "every ps_core_open_impl() failure return goes through
 * here"), so this tool captures stderr for that one call and treats its
 * presence as authoritative pass/fail -- no guessing from an exit code or
 * text elsewhere.
 *
 * A byte offset is only interesting if the *pristine* file (also run
 * through the same fixup call, unmutated) opens cleanly but the mutated-
 * then-fixed-up file does not: that is a fixup that is not doing its job
 * for that field, the exact class every one of rounds 1-7's Codex findings
 * on this file has been. Every such offset must be triaged into the
 * allowlist below as (b) a genuine semantic rejection or (c) a deliberately
 * unpinned identity field -- or fixed in fuzz_crc_fixup.c so it no longer
 * rejects at all.
 *
 * Usage:
 *   gate_sweep list                 -- run the sweep, print every rejected
 *                                       offset found, per target. Always
 *                                       exits 0 (a discovery tool, not a
 *                                       regression gate).
 *   gate_sweep check <allowlist>     -- run the sweep, compare every
 *                                       rejected offset against the given
 *                                       allowlist file (see
 *                                       gate_sweep_allowlist.txt's own
 *                                       header for its format), and fail
 *                                       (exit 1, listing exactly what is
 *                                       new) if any rejection is not on
 *                                       it. This is what the meson
 *                                       'pagestore_gate_sweep' test runs.
 *
 * Not part of the ordinary build (like the rest of contrib/pagestore/fuzz/,
 * this only builds when explicitly requested); see fuzz/FIXUP_GATES.md for
 * how to build and rerun it by hand.
 *
 *-------------------------------------------------------------------------
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pagestore_core.h"
#include "fuzz_common.h"
#include "fuzz_crc_fixup.h"

/*
 * How much of the file's front is swept per target. Large enough to cover
 * every fixed header this file's fixups know about (the biggest is
 * forkmeta_snapshot_manifest/walidx_snapshot_manifest's 80/64-byte header)
 * plus several records of every record-structured format currently in
 * this table (56-80 bytes/record for forkmeta/retention/timelines/walidx
 * WIDX records; the one 1080-byte WIPG shape is covered by its own header
 * fields, all within the first 24 bytes). Kept deliberately short of the
 * full walidx progress record or the multi-KB image-layer index/footer --
 * see FIXUP_GATES.md for why those are covered by targeted, not brute-
 * force, checks.
 */
#define GATE_SWEEP_BYTES_DEFAULT 256

/*
 * How much of the file's front this run sweeps, and how many of the two
 * flip masks (0xFF, then 0x01) it tries per offset -- both overridable
 * (smaller) from the command line. A full run (the default, for manual
 * `list` audits -- see this file's header comment) costs one
 * ps_core_open()/maintenance()/close() cycle per (offset, mask) pair,
 * ~3800 of them across all 21 targets; each cycle's cost is dominated by
 * the product's own allocator traffic (memtables, page cache, hash
 * tables, ...), which glibc's MALLOC_PERTURB_ (meson's test harness sets
 * it on every test, to catch use-after-free) makes roughly an order of
 * magnitude slower than a plain interactive run -- comfortably under a
 * minute unperturbed, minutes long under it. The meson-wired regression
 * test (pagestore_gate_sweep) therefore passes a smaller sweep width to
 * stay well inside its timeout; anyone rerunning the full audit by hand
 * (see FIXUP_GATES.md) gets the full 256 bytes x 2 masks by default.
 */
static size_t g_sweep_bytes = GATE_SWEEP_BYTES_DEFAULT;
static unsigned g_mask_count = 2;

/* Per-target seed source for the one pristine-template-less target that
 * needs a real file on disk rather than a synthetic buffer (see
 * fuzz_common.c's own comment on page_segment/walidx_log_epoch; the latter
 * builds its seed in memory in load_seed() below and needs no path at
 * all). Relative to pagestore_dir (this program's required first-after-
 * mode argument, e.g. ".../contrib/pagestore") -- mirrors the same
 * fallback the throwaway PR #303 probes have used all along. */
#define PAGE_SEGMENT_FALLBACK_SEED \
	"fuzz/corpus/page_segment/posix-mvp-baseline_seg_00000000"

static char work_dir[4096];
static char pagestore_dir[4096];

static void
find_work_dir(const char *scratch_dir)
{
	DIR		   *d = opendir(scratch_dir);
	struct dirent *ent;

	if (d == NULL)
	{
		fprintf(stderr, "gate_sweep: opendir %s: %s\n", scratch_dir,
				strerror(errno));
		exit(1);
	}
	while ((ent = readdir(d)) != NULL)
	{
		if (strncmp(ent->d_name, "psfuzz-work-", 12) == 0)
		{
			snprintf(work_dir, sizeof(work_dir), "%s/%s", scratch_dir,
					 ent->d_name);
			closedir(d);
			return;
		}
	}
	closedir(d);
	fprintf(stderr, "gate_sweep: no psfuzz-work-* under %s\n", scratch_dir);
	exit(1);
}

static void
write_file(const char *path, const uint8_t *data, size_t len)
{
	int			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	size_t		off = 0;

	if (fd < 0)
	{
		fprintf(stderr, "gate_sweep: open %s: %s\n", path, strerror(errno));
		exit(1);
	}
	while (off < len)
	{
		ssize_t		n = write(fd, data + off, len - off);

		if (n <= 0)
		{
			fprintf(stderr, "gate_sweep: write %s failed\n", path);
			exit(1);
		}
		off += (size_t) n;
	}
	close(fd);
}

/*
 * Loads target `name`'s seed bytes: the real pristine template when one
 * exists, else the same documented fallback the round-6 probes used.
 * Returns 0 and fills the output buffer/length on success (caller frees
 * it), -1 if this target genuinely has nothing to seed a sweep from.
 */
static int
load_seed(const char *name, const char *relpath, uint8_t **buf, size_t *len)
{
	const uint8_t *tmpl;
	size_t		tmpl_len = 0;

	tmpl = ps_fuzz_template_lookup(relpath, &tmpl_len);
	if (strcmp(name, "store_config") != 0 && tmpl != NULL && tmpl_len > 0)
	{
		*buf = malloc(tmpl_len);
		memcpy(*buf, tmpl, tmpl_len);
		*len = tmpl_len;
		return 0;
	}

	if (strcmp(name, "walidx_log_epoch") == 0)
	{
		/* See fuzz_crc_fixup.c's fixup_walidx_log_epoch_watermark() header
		 * comment and FIXUP_GATES.md's walidx_log_epoch row: the pristine
		 * template is genuinely empty, and every checked-in corpus seed
		 * for it is a deliberate wrong-timeline cross-seed. Build the
		 * minimal, genuinely valid (timeline 0) synthetic WalIdxRec the
		 * round-6 probes used instead: magic/rec_len=64, flags=0,
		 * end_lsn=0 (walidx_metadata_valid()'s trivial case), timeline=0,
		 * crc recomputed.
		 */
		uint8_t    *b = calloc(1, 64);
		uint32_t	crc = 2166136261u;
		size_t		i;

		b[0] = 0x58;
		b[1] = 0x44;
		b[2] = 0x49;
		b[3] = 0x57;			/* "WIDX" little-endian magic bytes */
		b[4] = 64;				/* rec_len */
		/* bytes 16-19 (timeline) already zero from calloc */
		for (i = 0; i < 64; i++)
		{
			if (i >= 8 && i < 12)
				continue;		/* crc field itself, filled below */
			crc ^= b[i];
			crc *= 16777619u;
		}
		b[8] = (uint8_t) crc;
		b[9] = (uint8_t) (crc >> 8);
		b[10] = (uint8_t) (crc >> 16);
		b[11] = (uint8_t) (crc >> 24);
		*buf = b;
		*len = 64;
		return 0;
	}

	if (strcmp(name, "page_segment") == 0 || strcmp(name, "store_config") == 0)
	{
		char		path[4096];
		FILE	   *f;
		long		flen;

		snprintf(path, sizeof(path), "%s/%s", pagestore_dir,
				 strcmp(name, "store_config") == 0 ?
				 "fuzz/corpus/store_config/posix-artifact-lifecycle" :
				 PAGE_SEGMENT_FALLBACK_SEED);
		f = fopen(path, "rb");
		if (f == NULL)
			return -1;
		fseek(f, 0, SEEK_END);
		flen = ftell(f);
		fseek(f, 0, SEEK_SET);
		*buf = malloc((size_t) flen);
		if (fread(*buf, 1, (size_t) flen, f) != (size_t) flen)
		{
			fclose(f);
			free(*buf);
			return -1;
		}
		fclose(f);
		*len = (size_t) flen;
		return 0;
	}

	return -1;
}

/*
 * Runs one full iteration for `name`: fixup(always) the buffer, write it
 * to work_dir/relpath, open+maintenance+close, capturing every byte of
 * stderr this one call produces. Returns 1 if the store opened (no
 * "open step" line appeared), 0 if it did not.
 */
static int
run_one(const char *name, const char *relpath, const uint8_t *seed,
		size_t len)
{
	uint8_t    *mutant = malloc(len);
	char		path[4096];
	char	   *captured = NULL;
	size_t		captured_len = 0;
	FILE	   *capture;
	int			saved_stderr;
	int			opened;

	memcpy(mutant, seed, len);
	ps_fuzz_crc_fixup(name, work_dir, mutant, len);
	if (strcmp(name, "walidx_log_epoch") == 0)
		fixup_walidx_log_epoch_watermark(work_dir, (uint64_t) len);
	if (strcmp(name, "wal_segment") == 0 ||
		strcmp(name, "walidx_snapshot_manifest") == 0)
	{
		/* Mirror ps_fuzz_run_one()'s own pre-fixup resize (fuzz_common.c):
		 * this tool calls ps_fuzz_crc_fixup() directly rather than through
		 * that function, so it must redo the resize itself, in the same
		 * order (resize, then fixup) -- fixup_wal_segment() only derives a
		 * correct payload_len/segment_size once the buffer is already the
		 * fixed length. */
		size_t		want = strcmp(name, "wal_segment") == 0 ?
			ps_fuzz_wal_segment_fixed_len() :
			ps_fuzz_walidx_manifest_fixed_len();

		if (want > 0 && want != len)
		{
			uint8_t    *resized = realloc(mutant, want);

			if (resized != NULL)
			{
				if (want > len)
					memset(resized + len, 0, want - len);
				mutant = resized;
				len = want;
				/* Redo the fixup on the correctly sized buffer -- the
				 * first pass above ran on the wrong length. */
				ps_fuzz_crc_fixup(name, work_dir, mutant, len);
			}
		}
	}

	snprintf(path, sizeof(path), "%s/%s", work_dir, relpath);
	write_file(path, mutant, len);
	free(mutant);

	/*
	 * Capture via a real temp file and dup2(), the same fd-swap shape
	 * fuzz_common.c's own mute_output()/unmute_output() already use
	 * successfully for stdout -- not open_memstream(): a memstream FILE's
	 * fileno() is not a normal kernel fd other writers (the product's own
	 * fprintf(stderr, ...) calls, via the real stderr FILE*, after fd 2 is
	 * dup2'd) can actually deliver bytes into by writing to fd 2, so a
	 * first version of this using open_memstream() silently captured
	 * nothing and reported every mutation as "opened".
	 */
	{
		char		capture_path[] = "/tmp/psfuzz-gatesweep-capture-XXXXXX";
		int			capture_fd = mkstemp(capture_path);

		fflush(stderr);
		saved_stderr = dup(fileno(stderr));
		dup2(capture_fd, fileno(stderr));
		close(capture_fd);

		if (ps_core_open(work_dir) == 0)
		{
			(void) ps_core_maintenance();
			ps_core_close();
		}

		fflush(stderr);
		dup2(saved_stderr, fileno(stderr));
		close(saved_stderr);

		capture = fopen(capture_path, "rb");
		if (capture != NULL)
		{
			fseek(capture, 0, SEEK_END);
			captured_len = (size_t) ftell(capture);
			fseek(capture, 0, SEEK_SET);
			captured = malloc(captured_len + 1);
			if (fread(captured, 1, captured_len, capture) != captured_len)
				captured_len = 0;
			captured[captured_len] = '\0';
			fclose(capture);
		}
		unlink(capture_path);
	}

	opened = (captured == NULL) ||
		(strstr(captured, "open step ") == NULL);
	free(captured);

	/*
	 * A successful open+maintenance()+close() above can itself have
	 * changed other files in work_dir (a checkpoint, a compacted
	 * manifest, ...) -- without undoing that, later iterations (for this
	 * target or, worse, the next one) would run against an increasingly
	 * drifted store instead of the pristine fixture, masking real gates.
	 * ps_fuzz_run_one() gets this from its own reset_work_dir() call after
	 * every iteration; this tool drives ps_core_open() directly, so it
	 * must call the same reset explicitly (see ps_fuzz_reset_work_dir()'s
	 * own comment in fuzz_common.c/.h).
	 */
	ps_fuzz_reset_work_dir();
	return opened;
}

typedef struct Rejection
{
	const char *target;
	long		offset;
	unsigned	mask;
} Rejection;

static Rejection rejections[8192];
static int	rejection_count = 0;

static void
record_rejection(const char *target, long offset, unsigned mask)
{
	if (rejection_count >= (int) (sizeof(rejections) / sizeof(rejections[0])))
		return;
	rejections[rejection_count].target = target;
	rejections[rejection_count].offset = offset;
	rejections[rejection_count].mask = mask;
	rejection_count++;
}

static void
sweep_target(const char *name, const char *relpath)
{
	uint8_t    *seed;
	size_t		len;
	size_t		sweep_len;
	size_t		off;
	unsigned	masks[2] = {0xFF, 0x01};
	unsigned	m;

	if (load_seed(name, relpath, &seed, &len) != 0)
	{
		fprintf(stderr, "gate_sweep: %-30s SKIP (no seed available)\n", name);
		if (strcmp(name, "store_config") == 0 ||
			strcmp(name, "walidx_snapshot_manifest") == 0)
			exit(1);
		return;
	}

	if (strcmp(name, "store_config") == 0 &&
		(len < 5 || memcmp(seed, "PSS2 ", 5) != 0))
	{
		fprintf(stderr, "gate_sweep: store_config requires a PSS2 seed\n");
		exit(1);
	}

	if (!run_one(name, relpath, seed, len))
	{
		fprintf(stderr,
				"gate_sweep: %-30s SKIP (pristine itself does not open "
				"under fixup -- see FIXUP_GATES.md, not this sweep)\n",
				name);
		free(seed);
		if (strcmp(name, "store_config") == 0 ||
			strcmp(name, "walidx_snapshot_manifest") == 0)
			exit(1);
		return;
	}

	/* Length mutations must still open with the fixture's shard set. */
	if (strcmp(name, "walidx_snapshot_manifest") == 0)
	{
		uint8_t    *extended = calloc(1, len + 16);

		memcpy(extended, seed, len);
		if (!run_one(name, relpath, extended, len + 16) ||
			!run_one(name, relpath, seed, len - 16))
		{
			fprintf(stderr, "gate_sweep: manifest resize failed\n");
			exit(1);
		}
		free(extended);
	}

	sweep_len = len < g_sweep_bytes ? len : g_sweep_bytes;
	{
		int			before = rejection_count;

		for (off = 0; off < sweep_len; off++)
		{
			for (m = 0; m < g_mask_count; m++)
			{
				uint8_t    *mutated = malloc(len);
				int			ok;

				memcpy(mutated, seed, len);
				mutated[off] ^= masks[m];
				ok = run_one(name, relpath, mutated, len);
				free(mutated);
				if (!ok)
					record_rejection(name, (long) off, masks[m]);
			}
		}
		free(seed);
		printf("%-30s swept %zu bytes x2 masks, %d rejection(s)\n", name,
			   sweep_len, rejection_count - before);
	}
}

static int
allowed(const char *allowlist_path, const char *target, long offset,
		unsigned mask)
{
	FILE	   *f = fopen(allowlist_path, "r");
	char		line[512];
	int			found = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL)
	{
		char		tgt[128];
		long		off;
		unsigned	msk;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%127s %ld 0x%x", tgt, &off, &msk) != 3)
			continue;
		if (strcmp(tgt, target) == 0 && off == offset && msk == mask)
		{
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Mutate each rec_len in both 448-byte shapes and mixed logs. A repair
 * must preserve all other bytes and produce the pristine repaired log. */
static int
timeline_framing_regression(void)
{
	const size_t shapes[][8] = {
		{64, 64, 64, 64, 64, 64, 64, 0},
		{56, 56, 56, 56, 56, 56, 56, 56},
		{56, 64, 56, 64, 0, 0, 0, 0},
		{64, 56, 64, 56, 0, 0, 0, 0}
	};
	const uint32_t bad_lengths[] = {0, 55, 65, UINT32_MAX};
	unsigned char pristine[448], expected[448], mutated[448];

	for (size_t shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++)
	{
		size_t len = 0;

		memset(pristine, 0, sizeof(pristine));
		for (size_t record = 0; record < 8 && shapes[shape][record]; record++)
		{
			uint32_t magic = 0x12345678;
			uint32_t stride = shapes[shape][record];
			uint32_t id = record + 1;

			memcpy(pristine + len, &magic, 4);
			memcpy(pristine + len + 4, &stride, 4);
			memcpy(pristine + len + 12, &id, 4);
			len += stride;
		}
		memcpy(expected, pristine, len);
		ps_fuzz_crc_fixup("timelines", work_dir, expected, len);
		for (size_t off = 0, record = 0; off < len; off += shapes[shape][record++])
			for (size_t bad = 0; bad < sizeof(bad_lengths) / sizeof(bad_lengths[0]); bad++)
			{
				memcpy(mutated, pristine, len);
				memcpy(mutated + off + 4, &bad_lengths[bad], 4);
				ps_fuzz_crc_fixup("timelines", work_dir, mutated, len);
				if (memcmp(mutated, expected, len) != 0)
				{
					fprintf(stderr, "FAIL: timeline framing shape=%zu offset=%zu bad=%u\n",
							shape, off, bad_lengths[bad]);
					return 0;
				}
			}
	}
	return 1;
}

/* WIPG framing mutations must not shift later mixed-version records. */
static int
horizon_framing_regression(void)
{
	unsigned char pristine[2176], expected[2176], mutated[2176];
	const uint32_t bad_lengths[] = {0, 55, UINT32_MAX};

	for (unsigned shape = 0; shape < 4; shape++)
	{
		uint32_t first = (shape & 1) ? 1088 : 1080;
		uint32_t second = (shape & 2) ? 1088 : 1080;
		uint32_t magic = 0x57495047;
		size_t len = first + second;

		memset(pristine, 0, sizeof(pristine));
		memcpy(pristine, &magic, 4);
		memcpy(pristine + 4, &first, 4);
		memcpy(pristine + first, &magic, 4);
		memcpy(pristine + first + 4, &second, 4);
		memcpy(expected, pristine, len);
		ps_fuzz_crc_fixup("walidx_log_legacy", work_dir, expected, len);
		for (unsigned record = 0; record < 2; record++)
			for (unsigned bad = 0; bad < 3; bad++)
			{
				size_t off = record ? first : 0;

				memcpy(mutated, pristine, len);
				memcpy(mutated + off + 4, &bad_lengths[bad], 4);
				ps_fuzz_crc_fixup("walidx_log_legacy", work_dir, mutated, len);
				if (memcmp(mutated, expected, len) != 0)
				{
					fprintf(stderr, "FAIL: horizon framing shape=%u record=%u bad=%u\n",
							shape, record, bad_lengths[bad]);
					return 0;
				}
			}
	}
	return 1;
}

int
main(int argc, char **argv)
{
	char		scratch_template[] = "/tmp/psfuzz-gatesweep-XXXXXX";
	char	   *scratch;
	int			i;
	int			mode_check;
	const char *allowlist_path = NULL;

	if (argc < 2 || (strcmp(argv[1], "list") != 0 &&
					 strcmp(argv[1], "check") != 0))
	{
		fprintf(stderr,
				"usage: %s list <pagestore-dir> [sweep-bytes [mask-count]]\n"
				"       %s check <allowlist-file> <pagestore-dir> "
				"[sweep-bytes [mask-count]]\n",
				argv[0], argv[0]);
		return 2;
	}
	mode_check = strcmp(argv[1], "check") == 0;
	if (mode_check)
	{
		if (argc < 4 || argc > 6)
		{
			fprintf(stderr,
					"usage: %s check <allowlist-file> <pagestore-dir> "
					"[sweep-bytes [mask-count]]\n", argv[0]);
			return 2;
		}
		allowlist_path = argv[2];
		snprintf(pagestore_dir, sizeof(pagestore_dir), "%s", argv[3]);
		if (argc >= 5)
			g_sweep_bytes = (size_t) atol(argv[4]);
		if (argc >= 6)
			g_mask_count = (unsigned) atoi(argv[5]);
	}
	else
	{
		if (argc < 3 || argc > 5)
		{
			fprintf(stderr,
					"usage: %s list <pagestore-dir> [sweep-bytes [mask-count]]\n",
					argv[0]);
			return 2;
		}
		snprintf(pagestore_dir, sizeof(pagestore_dir), "%s", argv[2]);
		if (argc >= 4)
			g_sweep_bytes = (size_t) atol(argv[3]);
		if (argc >= 5)
			g_mask_count = (unsigned) atoi(argv[4]);
	}
	if (g_mask_count < 1)
		g_mask_count = 1;
	if (g_mask_count > 2)
		g_mask_count = 2;

	scratch = mkdtemp(scratch_template);
	if (scratch == NULL)
	{
		perror("mkdtemp");
		return 1;
	}
	setenv("TMPDIR", scratch, 1);
	/* Every run_one() call below goes through ps_fuzz_crc_fixup() directly
	 * (not ps_fuzz_run_one()/should_fixup_this_iteration()), so no
	 * PS_FUZZ_CRC_FIXUP mode selection applies here -- this tool always
	 * exercises the fixed-up half, by construction. */
	ps_fuzz_global_init();
	find_work_dir(scratch);
	if (!timeline_framing_regression() || !horizon_framing_regression())
		return 1;

	for (i = 0; i < ps_fuzz_target_count; i++)
		sweep_target(ps_fuzz_targets[i].name, ps_fuzz_targets[i].relpath);

	printf("\n%d total rejection(s) across %d target(s)\n", rejection_count,
		   ps_fuzz_target_count);

	if (!mode_check)
	{
		for (i = 0; i < rejection_count; i++)
			printf("  %-30s offset=%-6ld mask=0x%02x\n",
				   rejections[i].target, rejections[i].offset,
				   rejections[i].mask);
		return 0;
	}

	{
		int			unexpected = 0;

		for (i = 0; i < rejection_count; i++)
		{
			if (!allowed(allowlist_path, rejections[i].target,
						 rejections[i].offset, rejections[i].mask))
			{
				printf("  UNEXPECTED  %-30s offset=%-6ld mask=0x%02x "
					   "(not on %s)\n", rejections[i].target,
					   rejections[i].offset, rejections[i].mask,
					   allowlist_path);
				unexpected++;
			}
		}
		if (unexpected > 0)
		{
			printf("FAIL: %d rejection(s) not on the allowlist -- pin the "
				   "field in fuzz_crc_fixup.c, or add a documented (b)/(c) "
				   "entry to %s\n", unexpected, allowlist_path);
			return 1;
		}
		printf("PASS: every rejection is on the documented allowlist\n");
		return 0;
	}
}
