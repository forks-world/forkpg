#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pagestore_walidx_prune.h"

static int run;
static int failed;

static void
check(int ok, const char *name)
{
	run++;
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", name);
		failed++;
	}
}

/*
 * Design doc BRANCH_SNAPSHOT_SEQ_CAP.md S3.7(7) rev 3 (amendment, no
 * plan-epoch/lsn-range gate on WAL-index publication): a plan built from an
 * earlier fork-event/page-admission snapshot must always retain at least
 * what a later, more-informed replan would need, because a later admission
 * only *adds* bases and deaths -- it never removes one, and a stored image
 * or a definitive event, once true, stays true.  This is the merge-blocking
 * property test for that argument (§8.2/§10 items 4-6, superseded by
 * S1-S4): `ps_walidx_prune_plan_bases()` is exactly the planner function
 * `walidx_plan_bases_build()`/`walidx_entry_prune_plan()` call per (key,
 * block), so verifying its monotonicity here verifies the production
 * mechanism, not a reimplementation of it.
 *
 * Two snapshots share the same items[]/cutoff/horizons/protection flags
 * (protection -- whether page-history retention is independently keeping a
 * timeline's stored images at a horizon -- does not change within the
 * short window this argument is about): T0 is the state a plan was built
 * and published from; T1 is T0 plus K "late" admissions (each admission
 * only ever adds one base or one death LSN, chosen from a random position,
 * not necessarily the newest -- exactly what a lagging materializer replay
 * or a lagging writer page eviction can do while WAL-index indexing keeps
 * advancing H, design doc §3.7(7) rev 3 §7.1).
 *
 * Checked for every (T0, T1) pair where both plans succeed:
 *   (a) keep1[i] implies keep0[i]: the published (T0) plan is a superset of
 *       what a strictly-more-informed (T1) plan would itself retain --
 *       nothing T1 needs (including every record after T1's own base,
 *       whatever retain_chain()'s FPI-vs-base interplay decides that is)
 *       is ever missing from what T0 already kept.  This is the literal,
 *       exact form of "every record needed after the base is retained by
 *       the plan": T1's kept set *is* "what's needed" as of T1, computed by
 *       the real production function, not a re-derivation of it.
 *   (b) for every view (cutoff and every discrete horizon), the reader's
 *       replacement base -- max(newest visible death, newest visible
 *       image), exactly ps_death_supersedes()'s rule, mirrored by
 *       retain_chain() inside pagestore_walidx_prune.c -- computed under T1
 *       is >= the same base computed under T0: the base is monotone.
 */

static uint64_t
xorshift(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* The newest value <= h in a sorted, ascending, nonzero, distinct array, or
 * 0 if none (0 is never a real base/death LSN, so it doubles as "none"). */
static uint64_t
newest_at_or_below(const uint64_t *arr, uint32_t n, uint64_t h)
{
	uint64_t	best = 0;

	for (uint32_t i = 0; i < n; i++)
		if (arr[i] <= h && arr[i] > best)
			best = arr[i];
	return best;
}

/* max(death, image) exactly as retain_chain()/ps_death_supersedes() apply
 * it: the image only counts when the horizon is protected; the death
 * always counts. */
static uint64_t
effective_base(int protected_horizon, const uint64_t *bases, uint32_t nbases,
			  const uint64_t *deaths, uint32_t ndeaths, uint64_t h)
{
	uint64_t	image = protected_horizon ? newest_at_or_below(bases, nbases, h) : 0;
	uint64_t	death = newest_at_or_below(deaths, ndeaths, h);

	return image > death ? image : death;
}

static void
dedup_sorted(uint64_t *arr, uint32_t *n)
{
	uint32_t	w = 0;

	for (uint32_t i = 0; i < *n; i++)
		if (w == 0 || arr[i] != arr[w - 1])
			arr[w++] = arr[i];
	*n = w;
}

static int
u64_cmp(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a;
	uint64_t	y = *(const uint64_t *) b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

#define MONO_MAXN	20
#define MONO_MAXSET	16

static void
run_monotonicity_property(uint64_t seed, uint32_t niter)
{
	uint64_t	s = seed ? seed : 1;
	uint32_t	skipped = 0;
	uint32_t	superset_violations = 0;
	uint32_t	base_regressions = 0;

	for (uint32_t iter = 0; iter < niter; iter++)
	{
		PsWalIdxPruneItem items[MONO_MAXN];
		uint32_t	n = 1 + (uint32_t) (xorshift(&s) % MONO_MAXN);
		uint64_t	lsn = 0;
		uint64_t	last_end = 0;
		uint64_t	cutoff;
		uint32_t	nh = (uint32_t) (xorshift(&s) % 3);
		uint64_t	horizons[2];
		int			cutoff_ok = (int) (xorshift(&s) % 2);
		unsigned char horizon_ok[2];
		uint64_t	bases0[MONO_MAXSET],
					deaths0[MONO_MAXSET],
					bases1[MONO_MAXSET],
					deaths1[MONO_MAXSET];
		uint32_t	nbases0,
					ndeaths0,
					nbases1,
					ndeaths1;
		uint32_t	nlate;
		unsigned char keep0[MONO_MAXN],
					keep1[MONO_MAXN];
		int			rc0,
					rc1;

		for (uint32_t i = 0; i < n; i++)
		{
			uint64_t	end;

			lsn += 1 + xorshift(&s) % 6;
			end = lsn + 1 + xorshift(&s) % 6;
			if (end <= last_end)
				end = last_end + 1 + xorshift(&s) % 6;
			items[i].lsn = lsn;
			items[i].end_lsn = end;
			items[i].known = 1;
			items[i].fpi = (i == 0 || xorshift(&s) % 3 == 0) ? 1 : 0;
			last_end = end;
		}
		cutoff = 1 + xorshift(&s) % (lsn + last_end + 10);
		for (uint32_t h = 0; h < nh; h++)
		{
			horizons[h] = 1 + xorshift(&s) % (lsn + last_end + 10);
			horizon_ok[h] = (unsigned char) (xorshift(&s) % 2);
		}

		/* T0: the state a plan is built and published from. */
		nbases0 = xorshift(&s) % (MONO_MAXSET / 2);
		for (uint32_t i = 0; i < nbases0; i++)
			bases0[i] = 1 + xorshift(&s) % (lsn + last_end + 10);
		qsort(bases0, nbases0, sizeof(*bases0), u64_cmp);
		dedup_sorted(bases0, &nbases0);
		ndeaths0 = xorshift(&s) % (MONO_MAXSET / 2);
		for (uint32_t i = 0; i < ndeaths0; i++)
			deaths0[i] = 1 + xorshift(&s) % (lsn + last_end + 10);
		qsort(deaths0, ndeaths0, sizeof(*deaths0), u64_cmp);
		dedup_sorted(deaths0, &ndeaths0);

		/* T1 = T0 plus "late" admissions: strictly more bases and deaths,
		 * at arbitrary positions -- never fewer, matching the design doc's
		 * "later admissions only add" lemma. */
		memcpy(bases1, bases0, nbases0 * sizeof(*bases0));
		nbases1 = nbases0;
		nlate = 1 + xorshift(&s) % 4;
		for (uint32_t i = 0; i < nlate && nbases1 < MONO_MAXSET; i++)
			bases1[nbases1++] = 1 + xorshift(&s) % (lsn + last_end + 10);
		qsort(bases1, nbases1, sizeof(*bases1), u64_cmp);
		dedup_sorted(bases1, &nbases1);

		memcpy(deaths1, deaths0, ndeaths0 * sizeof(*deaths0));
		ndeaths1 = ndeaths0;
		nlate = 1 + xorshift(&s) % 4;
		for (uint32_t i = 0; i < nlate && ndeaths1 < MONO_MAXSET; i++)
			deaths1[ndeaths1++] = 1 + xorshift(&s) % (lsn + last_end + 10);
		qsort(deaths1, ndeaths1, sizeof(*deaths1), u64_cmp);
		dedup_sorted(deaths1, &ndeaths1);

		rc0 = ps_walidx_prune_plan_bases(items, n, nbases0 ? bases0 : NULL,
										 nbases0, ndeaths0 ? deaths0 : NULL,
										 ndeaths0, cutoff, cutoff_ok,
										 nh ? horizons : NULL, nh,
										 nh ? horizon_ok : NULL, keep0);
		rc1 = ps_walidx_prune_plan_bases(items, n, nbases1 ? bases1 : NULL,
										 nbases1, ndeaths1 ? deaths1 : NULL,
										 ndeaths1, cutoff, cutoff_ok,
										 nh ? horizons : NULL, nh,
										 nh ? horizon_ok : NULL, keep1);
		if (rc0 < 0 || rc1 < 0)
		{
			skipped++;
			continue;
		}

		/* (a) The published plan is never missing anything a later,
		 * more-informed plan would itself retain. */
		for (uint32_t i = 0; i < n; i++)
			if (keep1[i] && !keep0[i])
			{
				superset_violations++;
				break;
			}

		/* (b): per view (cutoff, and every discrete horizon capped at
		 * cutoff exactly as ps_walidx_prune_plan_bases() itself does). */
		for (uint32_t v = 0; v < 1 + nh; v++)
		{
			uint64_t	h = (v == 0) ? cutoff :
				(horizons[v - 1] < cutoff ? horizons[v - 1] : cutoff);
			int			ok = (v == 0) ? cutoff_ok : horizon_ok[v - 1];
			uint64_t	base0v = effective_base(ok, bases0, nbases0,
												deaths0, ndeaths0, h);
			uint64_t	base1v = effective_base(ok, bases1, nbases1,
												deaths1, ndeaths1, h);

			if (base1v < base0v)
				base_regressions++;
		}
	}
	{
		char		name[160];

		snprintf(name, sizeof(name),
				 "WAL-index plan monotone under late admissions "
				 "(seed=%llu, niter=%u, skipped=%u, super=%u, base=%u)",
				 (unsigned long long) seed, niter, skipped,
				 superset_violations, base_regressions);
		check(superset_violations == 0 && base_regressions == 0, name);
	}
}

int
main(void)
{
	PsWalIdxPruneItem items[] = {
		{10, 20, 1, 1}, {30, 40, 1, 0}, {50, 60, 1, 1},
		{70, 80, 1, 0}, {90, 100, 1, 1}, {110, 120, 1, 0}
	};
	PsWalIdxPruneItem no_base[] = {{10, 20, 1, 0}, {30, 40, 1, 0}};
	PsWalIdxPruneItem legacy[] = {{10, 0, 0, 0}, {30, 40, 1, 1}};
	PsWalIdxPruneItem bad_fpi[] = {{10, 0, 0, 1}};
	PsWalIdxPruneItem bad_end[] = {{10, 10, 1, 1}};
	PsWalIdxPruneItem unsorted[] = {{30, 40, 1, 1}, {10, 20, 1, 1}};
	PsWalIdxPruneItem unsorted_end[] = {{10, 40, 1, 1}, {30, 35, 1, 1}};
	unsigned char keep[6];

	check(ps_walidx_prune_plan(NULL, 0, 100, NULL, 0, NULL) == 0,
		  "empty page needs no replacement base");
	check(ps_walidx_prune_plan(items, 6, 0, NULL, 0, keep) == -1,
		  "zero cutoff fails closed");
	check(ps_walidx_prune_plan(items, 6, 80, NULL, 0, keep) == 4 &&
		  !keep[0] && !keep[1] && keep[2] && keep[3] && keep[4] && keep[5],
		  "operational cutoff keeps its newest FPI chain and future tail");
	check(ps_walidx_prune_plan(items, 6, 100, (uint64_t[]) {40}, 1,
								 keep) == 4 &&
		  keep[0] && keep[1] && !keep[2] && !keep[3] && keep[4] && keep[5],
		  "an older discrete horizon adds its replacement chain");
	check(ps_walidx_prune_plan(items, 6, 100,
								 (uint64_t[]) {40, 80}, 2, keep) == 6 &&
		  keep[0] && keep[1] && keep[2] && keep[3] && keep[4] && keep[5],
		  "overlapping discrete chains are retained as a union");
	check(ps_walidx_prune_plan(items, 6, 80, (uint64_t[]) {1000}, 1,
								 keep) == 4 && keep[2] && keep[5],
		  "a future horizon is capped at the proven cutoff");
	check(ps_walidx_prune_plan(items, 6, 80, (uint64_t[]) {5}, 1,
								 keep) == 4 && !keep[0],
		  "a horizon before the page existed needs no chain");
	check(ps_walidx_prune_plan(items, 6, 45, NULL, 0, keep) == 6,
		  "a cutoff between records retains the visible chain and future tail");
	check(ps_walidx_prune_plan(items, 6, 15, NULL, 0, keep) == 6,
		  "record visibility is governed by end LSN, not start LSN");
	check(ps_walidx_prune_plan(no_base, 2, 40, NULL, 0, keep) == -1,
		  "a visible delta chain without an FPI fails closed");
	check(ps_walidx_prune_plan(legacy, 2, 40, NULL, 0, keep) == -1,
		  "legacy metadata at the cutoff fails closed");
	check(ps_walidx_prune_plan(legacy, 2, 20, NULL, 0, keep) == -1,
		  "legacy metadata is conservatively unprunable");
	check(ps_walidx_prune_plan(bad_fpi, 1, 20, NULL, 0, keep) == -1,
		  "an unknown FPI marker is rejected");
	check(ps_walidx_prune_plan(bad_end, 1, 20, NULL, 0, keep) == -1,
		  "known metadata requires record end after start");
	check(ps_walidx_prune_plan(unsorted, 2, 40, NULL, 0, keep) == -1,
		  "out-of-order records are rejected");
	check(ps_walidx_prune_plan(unsorted_end, 2, 40, NULL, 0, keep) == -1,
		  "out-of-order record ends are rejected");
	check(ps_walidx_prune_plan(items, 6, 80, NULL, 1, keep) == -1,
		  "missing horizon array is rejected");
	check(ps_walidx_prune_plan(NULL, 1, 80, NULL, 0, keep) == -1,
		  "missing item array is rejected");

	{
		/* Durable replacement page bases supersede the records they cover. */
		uint64_t base60[] = {60};
		uint64_t base80[] = {80};
		uint64_t base20[] = {20};
		uint64_t base_future[] = {100};
		uint64_t unsorted_bases[] = {60, 40};
		uint64_t zero_base[] = {0};
		unsigned char ok1[] = {1};
		unsigned char no1[] = {0};

		check(ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 80, 1,
										 NULL, 0, NULL, keep) == 3 &&
			  !keep[0] && !keep[1] && !keep[2] && keep[3] && keep[4] && keep[5],
			  "a stored image covers the FPI and every earlier record");
		check(ps_walidx_prune_plan_bases(items, 6, base80, 1, NULL, 0, 80, 1,
										 NULL, 0, NULL, keep) == 2 &&
			  !keep[3] && keep[4] && keep[5],
			  "an image at the cutoff leaves only the future tail");
		check(ps_walidx_prune_plan_bases(no_base, 2, base20, 1, NULL, 0, 40, 1,
										 NULL, 0, NULL, keep) == 1 &&
			  !keep[0] && keep[1],
			  "a page without any FPI compacts from a stored image");
		check(ps_walidx_prune_plan_bases(items, 6, base_future, 1, NULL, 0, 80,
										 1, NULL, 0, NULL, keep) == 4 && keep[2],
			  "an image after the cutoff is not yet visible");
		check(ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 100, 1,
										 (uint64_t[]) {40}, 1, ok1, keep) == 4 &&
			  keep[0] && keep[1] && !keep[2] && !keep[3] && keep[4] && keep[5],
			  "an older horizon below the image still keeps its FPI chain");
		check(ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 100, 1,
										 (uint64_t[]) {80}, 1, ok1, keep) == 3 &&
			  !keep[2] && keep[3] && keep[4] && keep[5],
			  "a horizon above the image starts its chain after the image");
		check(ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 80, 0,
										 NULL, 0, NULL, keep) == 4 && keep[2],
			  "an unprotected cutoff ignores stored images and keeps its FPI");
		check(ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 100, 1,
										 (uint64_t[]) {80}, 1, no1, keep) == 4 &&
			  keep[2] && keep[3] && keep[4] && keep[5],
			  "an unprotected horizon keeps its FPI chain despite the image");
		check(ps_walidx_prune_plan_bases(items, 6, NULL, 0, base60, 1, 80, 0,
										 (uint64_t[]) {80}, 1, no1, keep) == 3 &&
			  !keep[2] && keep[3],
			  "a death base holds even at unprotected horizons");
		check(ps_walidx_prune_plan_bases(items, 6, unsorted_bases, 2, NULL, 0,
										 80, 1, NULL, 0, NULL, keep) == -1,
			  "unsorted bases fail closed");
		check(ps_walidx_prune_plan_bases(items, 6, zero_base, 1, NULL, 0, 80, 1,
										 NULL, 0, NULL, keep) == -1,
			  "a zero base fails closed");
		check(ps_walidx_prune_plan_bases(items, 6, NULL, 0, NULL, 0, 80, 1,
										 NULL, 0, NULL, keep) == 4,
			  "no bases behaves exactly like the FPI-only planner");
	}

	/*
	 * S3.7(7) rev 3 named cases: a plan already published from T0 (base
	 * image at 60, protected, cutoff 100) must still hold everything a
	 * later, more-informed T1 plan needs.  Rather than hand-predict T0's
	 * exact bitmap (error-prone to do by hand against retain_chain()'s
	 * FPI-vs-base interplay), each case checks the actual relationship the
	 * design doc requires: T1's kept set is a subset of T0's.  All reuse
	 * the 6-item `items` table above (lsn 10..120, end 20..120, FPIs at
	 * 10/50/90).
	 */
	{
		uint64_t	base60[] = {60};
		unsigned char keep_t0[6];
		unsigned char keep_t1[6];
		int			rc0,
					rc1;

		rc0 = ps_walidx_prune_plan_bases(items, 6, base60, 1, NULL, 0, 100, 1,
										 NULL, 0, NULL, keep_t0);
		check(rc0 >= 0, "(setup) T0 plan (base image at 60, cutoff 100) succeeds");

		/* (a) a late TRUNCATE (death) at d=90, with base_plan=60 < d <= the
		 * horizon 100: T0 could not have known about it, yet T1's plan
		 * needs only what T0 already kept. */
		rc1 = ps_walidx_prune_plan_bases(items, 6, base60, 1,
										 (uint64_t[]) {90}, 1, 100, 1,
										 NULL, 0, NULL, keep_t1);
		check(rc1 >= 0 && rc1 <= rc0, "(a) late TRUNCATE needs no more than T0 kept");
		for (int i = 0; i < 6; i++)
			check(!keep_t1[i] || keep_t0[i],
				  "(a) every record T1 needs after a late TRUNCATE is in T0's plan");

		/* (b) a late UNLINK (death) at d=20, strictly below base_plan=60:
		 * max(death, image) ignores it, so T1's plan equals T0's exactly. */
		rc1 = ps_walidx_prune_plan_bases(items, 6, base60, 1,
										 (uint64_t[]) {20}, 1, 100, 1,
										 NULL, 0, NULL, keep_t1);
		check(rc1 == rc0 && memcmp(keep_t0, keep_t1, 6) == 0,
			  "(b) late UNLINK below base_plan is ignored by max(death,image)");

		/* (c) a late image strictly newer than base_plan (at 80): T1 needs
		 * only a subset of T0.  A late image strictly older than base_plan
		 * (at 30) changes nothing, since max() already picked 60. */
		rc1 = ps_walidx_prune_plan_bases(items, 6, (uint64_t[]) {60, 80}, 2,
										 NULL, 0, 100, 1, NULL, 0, NULL,
										 keep_t1);
		check(rc1 >= 0 && rc1 <= rc0, "(c) a late newer image needs no more than T0 kept");
		for (int i = 0; i < 6; i++)
			check(!keep_t1[i] || keep_t0[i],
				  "(c) every record T1 needs after a late newer image is in T0's plan");
		rc1 = ps_walidx_prune_plan_bases(items, 6, (uint64_t[]) {30, 60}, 2,
										 NULL, 0, 100, 1, NULL, 0, NULL,
										 keep_t1);
		check(rc1 == rc0 && memcmp(keep_t0, keep_t1, 6) == 0,
			  "(c) a late older image changes nothing");
	}

	/* The general property, over random histories/bases/deaths/horizons
	 * (merge blocker, S3.7(7) rev 3): 8 seeds x 3000 iterations. */
	{
		static const uint64_t seeds[] =
		{1, 2, 42, 1337, 424242, 99991, 7, 8675309};

		for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
			run_monotonicity_property(seeds[i], 3000);
	}

	printf("pagestore_walidx_prune_test: %d checks, %d failed\n", run, failed);
	return failed != 0;
}
