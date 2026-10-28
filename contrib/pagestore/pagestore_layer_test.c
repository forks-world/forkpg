/*-------------------------------------------------------------------------
 *
 * pagestore_layer_test.c
 *	  Standalone unit test for the immutable image-layer file format
 *	  (pagestore_layer.c writer/reader over the local layer store).  Runs with
 *	  no daemon and no PostgreSQL.
 *
 * Usage: pagestore_layer_test
 * Exit status: 0 = all checks passed, 1 = one or more failed.
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pagestore_layer.h"
#include "pagestore_layer_store.h"

static int	run = 0,
			failed = 0;

static void
check(int cond, const char *msg)
{
	run++;
	if (!cond)
	{
		failed++;
		fprintf(stderr, "  FAIL: %s\n", msg);
	}
}

#define PSZ 8192

/* A generated 32-MiB layer must never require whole-payload I/O. */
static uint32_t stream_write_max;
static uint32_t stream_read_max;
static uint32_t stream_pages;
static int	stream_fail_after = -1;

static int
bounded_write(uint64_t id, uint64_t off, const void *buf, uint32_t len)
{
	if (len > stream_write_max)
		stream_write_max = len;
	if (len > 256 * 1024)
		return -1;
	return PsLayerStoreLocal.write_local_layer_at(id, off, buf, len);
}

static int
bounded_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	/* Index metadata remains proportional to record count. */
	if (off < (uint64_t) 4096 * PSZ)
	{
		if (len > stream_read_max)
			stream_read_max = len;
		if (len > 256 * 1024)
			return -1;
	}
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}
static int
generated_page(const PsImgRec *rec, void *page, void *arg)
{
	(void) arg;
	if (stream_fail_after >= 0 && stream_pages >= (uint32_t) stream_fail_after)
		return -1;
	stream_pages++;
	memset(page, (unsigned char) rec->block, PSZ);
	return 0;
}

static void
streaming_layer_test(void)
{
	PsLayerStore bounded = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsImgRec   *recs = calloc(4096, sizeof(*recs));
	PsLayerDesc layer;
	unsigned char page[PSZ];
	PsKey		key = {1, 1, 905, 0, PS_KLASS_RELATION};

	check(recs != NULL, "streaming metadata allocation");
	if (!recs)
		return;
	bounded.write_local_layer_at = bounded_write;
	bounded.read_layer_block = bounded_read;
	ps_layer_store = &bounded;
	for (uint32_t i = 0; i < 4096; i++)
	{
		recs[i].key = key;
		recs[i].block = 4095 - i;
		recs[i].lsn = 100;
	}
	check(ps_image_layer_write_stream(910, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) == 0,
		  "stream generated 32-MiB image without resident input pages");
	check(stream_pages == 4096 && stream_write_max <= 256 * 1024,
		  "writer fetches one page per record with bounded writes");
	check(ps_image_layer_verify_data(&layer, PSZ) == 0 &&
		  stream_read_max <= 256 * 1024,
		  "large layer checksum uses bounded reads");
	for (uint32_t block = 0; block < 4096; block += 4095)
	{
		check(ps_image_layer_lookup(&layer, &key, block, 100, UINT64_MAX,
									page, PSZ, NULL, NULL) == 1 &&
			  page[0] == (unsigned char) block && page[PSZ - 1] == (unsigned char) block,
			  "stream sorting preserves page bytes at both ends");
	}
	stream_pages = 0;
	stream_fail_after = 40;
	check(ps_image_layer_write_stream(911, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) != 0,
		  "source read failure rejects partially written image");
	check(ps_layer_store->layer_exists_local(911) == 0,
		  "source failure removes unpublished output immediately");
	for (uint64_t id = 912; id < 916; id++)
	{
		stream_pages = 0;
		check(ps_image_layer_write_stream(id, 0, recs, 4096, PSZ,
										  generated_page, NULL, &layer) != 0 &&
			  ps_layer_store->layer_exists_local(id) == 0,
			  "repeated source failures do not accumulate partial layers");
	}
	stream_fail_after = -1;
	check(ps_image_layer_write_stream(910, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) != 0 &&
		  ps_layer_store->layer_exists_local(910) == 1,
		  "failed create must preserve the existing layer");
	check(ps_image_layer_write_stream(911, 0, recs, 4096, PSZ,
									  generated_page, NULL, &layer) == 0 &&
		  ps_image_layer_verify_data(&layer, PSZ) == 0,
		  "retry can recreate and seal a cleaned-up output");
	ps_layer_store = saved;
	free(recs);
}

/* A sweep of tiny layers must not evict an expensive large index merely
 * because the fixed number of cache slots filled up. */
static unsigned large_index_reads;

static int
counted_index_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	if (layer->layer_id == 30000 && off == (uint64_t) 1024 * PSZ &&
		len == 1024 * sizeof(PsImgIndexEnt))
		large_index_reads++;
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}

static void
index_cache_pressure_test(void)
{
	PsLayerStore counted = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsImgRec *recs = calloc(1024, sizeof(*recs));
	PsLayerDesc large, small;
	PsKey key = {1, 1, 30000, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	int ok = 1;

	check(recs != NULL, "allocate cache pressure fixture");
	if (recs == NULL)
		return;
	ps_image_layer_cache_reset();
	counted.read_layer_block = counted_index_read;
	ps_layer_store = &counted;
	for (uint32_t i = 0; i < 1024; i++)
	{
		recs[i].key = key;
		recs[i].block = i;
		recs[i].lsn = 100;
		recs[i].page = page;
	}
	if (ps_image_layer_write(30000, 0, recs, 1024, PSZ, &large) != 0 ||
		ps_image_layer_lookup(&large, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
		ok = 0;
	for (uint64_t id = 30001; id <= 30130 && ok; id++)
	{
		if (ps_image_layer_write(id, 0, recs, 1, PSZ, &small) != 0 ||
			ps_image_layer_lookup(&small, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
			ok = 0;
	}
	if (ok && ps_image_layer_lookup(&large, &key, 0, 100, 0, page, PSZ, NULL, NULL) != 1)
		ok = 0;
	check(ok, "large and small layers remain readable through cache pressure");
	check(large_index_reads == 1, "small-index churn retains the large verified index");
	ps_image_layer_cache_reset();
	ps_layer_store = saved;
	free(recs);
}

/* The current-format index need not allocate a second full-size copy. */
static void *index_read_destination;

static int
capture_index_read(const PsLayerDesc *layer, uint64_t off, void *buf, uint32_t len)
{
	if (layer->layer_id == 30131 && off == PSZ && len == sizeof(PsImgIndexEnt))
		index_read_destination = buf;
	return PsLayerStoreLocal.read_layer_block(layer, off, buf, len);
}

static void
index_direct_read_test(void)
{
	PsLayerStore counted = PsLayerStoreLocal;
	const PsLayerStore *saved = ps_layer_store;
	PsKey key = {1, 1, 30131, 0, PS_KLASS_RELATION};
	unsigned char page[PSZ] = {0};
	PsImgRec rec = {.key = key, .block = 0, .lsn = 100, .page = page};
	PsLayerDesc layer;
	PsImgIndexEnt *index = NULL;
	uint32_t n = 0;
	int rc;

	counted.read_layer_block = capture_index_read;
	ps_layer_store = &counted;
	index_read_destination = NULL;
	rc = ps_image_layer_write(30131, 0, &rec, 1, PSZ, &layer);
	if (rc == 0)
		rc = ps_image_layer_read_index(&layer, &index, &n);
	check(rc == 0 && n == 1 && index == index_read_destination &&
		  index->key.relNumber == key.relNumber,
		  "current index is read and verified directly into the result buffer");
	free(index);
	ps_layer_store = saved;
}

int
main(void)
{
	char		dir[] = "/tmp/pslayertestXXXXXX";
	uint32_t	psz = PSZ;
	static unsigned char pg[5][PSZ];
	PsKey		k5 = {1, 1, 5, 0, PS_KLASS_RELATION};	/* relation 5, block 0 */
	PsKey		k6 = {1, 1, 6, 0, PS_KLASS_RELATION};	/* relation 6, block 0 */
	PsKey		k9 = {1, 1, 9, 0, PS_KLASS_RELATION};	/* absent */
	PsLayerDesc d;
	unsigned char out[PSZ];
	int			r;

	if (!mkdtemp(dir) || ps_layer_store->open(dir) != 0)
	{
		fprintf(stderr, "setup failed\n");
		return 2;
	}

	/* Upgraded stores retain sequence-zero tuples alongside same-LSN rewrites.
	 * Exact reads must find the old identity, including when another layer
	 * contains only the rewrite (which must be a miss rather than an error). */
	{
		PsLayerDesc legacy;
		PsImgRec recs[2] = {
			{.key = k5, .block = 0, .lsn = 100, .admission_seq = 0, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 100, .admission_seq = 9, .page = pg[1]},
		};
		uint64_t lsn, seq;

		memset(pg[0], 0x71, psz);
		memset(pg[1], 0x72, psz);
		check(ps_image_layer_write(700, 0, recs, 2, psz, &legacy) == 0,
			  "write legacy identity and same-LSN rewrite");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 0, out,
			  psz, &lsn, &seq) == 1 && lsn == 100 && seq == 0 && out[128] == 0x71,
			  "exact lookup preserves sequence-zero bytes");
		check(ps_image_layer_lookup(&legacy, &k5, 0, 100, 0, out,
			  psz, &lsn, &seq) == 1 && seq == 9 && out[128] == 0x72,
			  "uncapped lookup still selects newest rewrite");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 8, out,
			  psz, NULL, NULL) == 0, "missing exact identity does not substitute older tuple");
		check(ps_image_layer_write(701, 0, recs + 1, 1, psz, &legacy) == 0,
			  "write rewrite-only layer");
		check(ps_image_layer_lookup_exact(&legacy, &k5, 0, 100, 0, out,
			  psz, NULL, NULL) == 0, "rewrite-only layer misses sequence-zero identity");
	}

	/* versions (out of insertion order on purpose):
	 *   (5,0)@100=0xA1  (5,0)@200=0xA2  (5,0)@300=0xA3
	 *   (5,1)@150=0xB1  (6,0)@250=0xC1 */
	memset(pg[0], 0xA1, psz);
	memset(pg[1], 0xA2, psz);
	memset(pg[2], 0xA3, psz);
	memset(pg[3], 0xB1, psz);
	memset(pg[4], 0xC1, psz);
	{
		PsImgRec	recs[5] = {
			{.key = k5, .block = 0, .lsn = 100, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 300, .page = pg[2]},
			{.key = k5, .block = 0, .lsn = 200, .page = pg[1]},
			{.key = k5, .block = 1, .lsn = 150, .page = pg[3]},
			{.key = k6, .block = 0, .lsn = 250, .page = pg[4]},
		};
		PsImgIndexEnt *idx = NULL;
		uint32_t	nidx = 0;

		recs[1].growth_lsn = 275;
		recs[1].order_id = 44;
		recs[1].seg_id = 3;
		recs[1].seg_off = 1234;
		recs[1].flags = PS_IMG_REC_SEG_VALID | PS_IMG_REC_ORDERED;

		check(ps_image_layer_write(7, 0, recs, 5, psz, &d) == 0,
			  "write image layer");
		check(ps_image_layer_read_index(&d, &idx, &nidx) == 0 && nidx == 5,
			  "read image v3 index");
		if (idx && nidx == 5)
		{
			PsImgIndexEnt *e = NULL;

			for (uint32_t i = 0; i < nidx; i++)
				if (idx[i].lsn == 300)
					e = &idx[i];
			check(e && e->growth_lsn == 275 && e->order_id == 44 &&
				  e->seg_id == 3 && e->seg_off == 1234 &&
				  e->flags == (PS_IMG_REC_SEG_VALID | PS_IMG_REC_ORDERED),
				  "image v3 recovery metadata round-trips");
		}
		free(idx);
	}

	check(d.start_key.relNumber == 5 && d.end_key.relNumber == 6,
		  "layer key range");
	check(d.lsn_start == 100 && d.lsn_end == 300, "layer lsn range");

	/* newest version <= read_lsn */
	r = ps_image_layer_lookup(&d, &k5, 0, 250, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA2, "(5,0)@<=250 -> version 200");
	r = ps_image_layer_lookup(&d, &k5, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA3, "(5,0)@<=1000 -> version 300");
	r = ps_image_layer_lookup(&d, &k5, 0, 100, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xA1, "(5,0)@<=100 -> version 100 (exact)");
	r = ps_image_layer_lookup(&d, &k5, 0, 50, 0, out, psz, NULL, NULL);
	check(r == 0, "(5,0)@<=50 -> no version (older than oldest)");
	r = ps_image_layer_lookup(&d, &k5, 1, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xB1, "(5,1) -> version 150");
	r = ps_image_layer_lookup(&d, &k6, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 1 && out[0] == 0xC1, "(6,0) -> version 250");
	r = ps_image_layer_lookup(&d, &k9, 0, 1000, 0, out, psz, NULL, NULL);
	check(r == 0, "absent key -> no version");

	/* The cache holds indexes only: an unverified descriptor copy still
	 * checksums the data section, and emptying the cache (the layer store's
	 * close) makes the next lookup read the index from the file again. */
	{
		PsLayerDesc copy = d;
		unsigned char bad = 0,
					  good = 0xA1;
		int			fd;

		copy.data_verified = false;
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &bad, 1, 0) == 1 && close(fd) == 0,
			  "corrupt image bytes behind a cached index");
		r = ps_image_layer_lookup(&copy, &k6, 0, 1000, 0, out, psz, NULL, NULL);
		check(r < 0 && !copy.data_verified,
			  "a cached index does not vouch for the data section");
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &good, 1, 0) == 1 && close(fd) == 0,
			  "restore image bytes");
		ps_image_layer_cache_reset();
		r = ps_image_layer_lookup(&copy, &k6, 0, 1000, 0, out, psz, NULL, NULL);
		check(r == 1 && copy.data_verified && out[0] == 0xC1,
			  "after a cache reset the layer is read, verified and served again");
	}

	/* GC must force a fresh checksum even if an earlier lookup cached success. */
	{
		unsigned char bad = 0,
					  good = 0xA1;
		int			fd = open(d.locations[0].uri, O_WRONLY);

		check(fd >= 0 && pwrite(fd, &bad, 1, 0) == 1 && close(fd) == 0,
			  "corrupt image bytes after cached verification");
		check(ps_image_layer_verify_data(&d, psz) != 0,
			  "forced data verification detects post-read corruption");
		check(!d.data_verified, "failed forced verification clears cached success");
		fd = open(d.locations[0].uri, O_WRONLY);
		check(fd >= 0 && pwrite(fd, &good, 1, 0) == 1 && close(fd) == 0 &&
			  ps_image_layer_verify_data(&d, psz) == 0,
			  "restored image bytes pass forced verification");
	}

	/* Lookups go through a cached, binary-searched index: many keys written
	 * in unsorted order must resolve exactly as a full scan would. */
	{
		enum { NK = 300 };
		static unsigned char pages[NK][PSZ];
		static PsImgRec many[NK * 2];
		PsLayerDesc d2;
		int			ok = 1;

		for (uint32_t i = 0; i < NK; i++)
		{
			PsKey		k = k5;

			k.relNumber = 1000 + (i * 7919) % NK;	/* unsorted input */
			memset(pages[i], (int) (k.relNumber & 0xff), psz);
			many[2 * i] = (PsImgRec) {.key = k, .block = i % 3, .lsn = 500, .page = pages[i]};
			many[2 * i + 1] = (PsImgRec) {.key = k, .block = i % 3, .lsn = 400, .page = pg[0]};
		}
		check(ps_image_layer_write(77, 0, many, NK * 2, psz, &d2) == 0,
			  "write a many-key image layer");
		r = ps_image_layer_lookup(&d2, &k5, 0, 1000, 0, out, psz, NULL, NULL);
		check(r == 0, "a key of another cached layer is absent from this one");
		for (uint32_t i = 0; i < NK && ok; i++)
		{
			PsKey		k = k5;
			uint64_t	l = 0;

			k.relNumber = 1000 + i;
			for (uint32_t b = 0; b < 3; b++)
			{
				r = ps_image_layer_lookup(&d2, &k, b, 450, 0, out, psz, &l, NULL);
				if (r == 1 && (l != 400 || out[0] != 0xA1))
					ok = 0;
				r = ps_image_layer_lookup(&d2, &k, b, 1000, 0, out, psz, &l, NULL);
				if (r == 1 && (l != 500 || out[0] != (unsigned char) (k.relNumber & 0xff)))
					ok = 0;
				if (r < 0)
					ok = 0;
			}
		}
		check(ok, "every key resolves to its newest version through the cached index");
		{
			int			hits = 0;

			for (uint32_t i = 0; i < NK; i++)
			{
				PsKey		k = k5;

				k.relNumber = 1000 + i;
				for (uint32_t b = 0; b < 3; b++)
					hits += ps_image_layer_lookup(&d2, &k, b, 1000, 0, out, psz,
												  NULL, NULL) == 1;
			}
			check(hits == NK, "each written (key, block) is found exactly once");
		}
	}

	/* --- delta layer: ordered collect in an LSN range --- */
	{
		PsLayerDesc dd;
		PsDeltaRec	drecs[4] = {
			{k5, 0, 300, "D300", 4}, {k5, 0, 100, "D100", 4},
			{k5, 0, 200, "DD200", 5}, {k5, 1, 150, "E150", 4},
		};
		PsDeltaOut	outs[8];
		uint32_t	dn = 0;
		unsigned char dbuf[16];

		check(ps_delta_layer_write(8, 0, drecs, 4, &dd) == 0, "write delta layer");
		check(dd.kind == PS_LAYER_DELTA && dd.lsn_start == 100 &&
			  dd.lsn_end == 300, "delta layer kind + lsn range");

		/* (5,0) in (100, 300]: expect 200 then 300, ascending; 100 excluded */
		r = ps_delta_layer_collect(&dd, &k5, 0, 100, 300, outs, 8, &dn);
		check(r == 0 && dn == 2, "delta collect (100,300] -> 2 records");
		check(outs[0].lsn == 200 && outs[1].lsn == 300, "deltas in ascending LSN");
		check(ps_layer_store->read_layer_block(&dd, outs[0].data_off, dbuf,
											   outs[0].data_len) == 0 &&
			  outs[0].data_len == 5 && memcmp(dbuf, "DD200", 5) == 0,
			  "delta payload readable (200)");

		dn = 0;
		r = ps_delta_layer_collect(&dd, &k5, 0, 0, 1000, outs, 8, &dn);
		check(r == 0 && dn == 3, "delta collect (0,1000] -> all 3 of (5,0)");
		dn = 0;
		r = ps_delta_layer_collect(&dd, &k5, 1, 0, 1000, outs, 8, &dn);
		check(r == 0 && dn == 1 && outs[0].lsn == 150, "delta collect other block");
	}

	{
		PsLayerDesc huge;
		PsDeltaFooter foot;
		char		uri[PS_LAYER_URI_MAX];
		uint64_t	size = (uint64_t) UINT32_MAX + 1 + sizeof(foot);
		int			fd;

		memset(&huge, 0, sizeof(huge));
		memset(&foot, 0, sizeof(foot));
		foot.magic = PS_DELTA_MAGIC;
		foot.version = PS_DELTA_VERSION;
		foot.index_off = (uint64_t) UINT32_MAX + 1;
		check(ps_layer_store->create_local_layer(11, uri, sizeof(uri)) == 0,
			  "create oversized delta layer shell");
		fd = open(uri, O_WRONLY);
		check(fd >= 0 &&
			  pwrite(fd, &foot, sizeof(foot),
					 (off_t) (size - sizeof(foot))) == (ssize_t) sizeof(foot) &&
			  close(fd) == 0,
			  "write sparse oversized delta footer");
		huge.layer_id = 11;
		huge.kind = PS_LAYER_DELTA;
		huge.location_count = 1;
		huge.locations[0].tier = PS_LAYER_TIER_LOCAL_HOT;
		huge.locations[0].available = true;
		huge.locations[0].size = size;
		snprintf(huge.locations[0].uri, sizeof(huge.locations[0].uri), "%s", uri);
		check(ps_delta_layer_verify_data(&huge) != 0,
			  "delta verifier rejects sections above the 32-bit read limit");
		unlink(uri);
	}

	/* --- read plan: base image + ordered delta chain --- */
	{
		PsLayerDesc img,
					dl;
		PsLayerMap	map;
		PsReadPlan	plan;
		PsImgRec	irecs[2] = {
			{.key = k5, .block = 0, .lsn = 100, .page = pg[0]},
			{.key = k5, .block = 0, .lsn = 200, .page = pg[1]},
		};
		PsDeltaRec	drecs[3] = {
			{k5, 0, 150, "x150", 4}, {k5, 0, 250, "x250", 4}, {k5, 0, 300, "x300", 4},
		};

		check(ps_image_layer_write(9, 0, irecs, 2, psz, &img) == 0, "plan: image");
		check(ps_delta_layer_write(10, 0, drecs, 3, &dl) == 0, "plan: delta");
		ps_layer_map_init(&map);
		ps_layer_map_add(&map, &img);
		ps_layer_map_add(&map, &dl);

		/* @250: base=image@200 (0xA2); deltas in (200,250] = {250} */
		check(ps_read_plan_build(&map, 0, &k5, 0, 250, psz, &plan) == 0, "plan@250");
		check(plan.has_base && plan.base_lsn == 200 && plan.base[0] == 0xA2,
			  "plan@250 base = image@200");
		check(plan.ndelta == 1 && plan.deltas[0].lsn == 250 &&
			  memcmp(plan.deltas[0].bytes, "x250", 4) == 0, "plan@250 one delta");
		ps_read_plan_free(&plan);

		/* @300: base=image@200; deltas in (200,300] = {250,300} ascending */
		check(ps_read_plan_build(&map, 0, &k5, 0, 300, psz, &plan) == 0, "plan@300");
		check(plan.base_lsn == 200 && plan.ndelta == 2 &&
			  plan.deltas[0].lsn == 250 && plan.deltas[1].lsn == 300,
			  "plan@300 chain {250,300}");
		ps_read_plan_free(&plan);

		/* @180: base=image@100 (0xA1); deltas in (100,180] = {150} */
		check(ps_read_plan_build(&map, 0, &k5, 0, 180, psz, &plan) == 0, "plan@180");
		check(plan.base_lsn == 100 && plan.base[0] == 0xA1 && plan.ndelta == 1 &&
			  plan.deltas[0].lsn == 150, "plan@180 base@100 + delta150");
		ps_read_plan_free(&plan);

		ps_layer_map_free(&map);
	}

	streaming_layer_test();
	index_cache_pressure_test();
	index_direct_read_test();
	fprintf(stderr, "%d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
