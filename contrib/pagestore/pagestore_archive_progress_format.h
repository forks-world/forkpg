/* Portable encoding for a single archive-progress authority snapshot.
 * Encoding alone is not a durability protocol; the provider must atomically
 * publish and sync it before acknowledging a transaction. */
#ifndef PAGESTORE_ARCHIVE_PROGRESS_FORMAT_H
#define PAGESTORE_ARCHIVE_PROGRESS_FORMAT_H
#include <stdint.h>
#include <string.h>
#include "pagestore_archive_progress.h"
#define PS_ARCHIVE_PROGRESS_FORMAT_BYTES 104u
#define PS_ARCHIVE_PROGRESS_FORMAT_MAGIC UINT32_C(0x50534150)
#define PS_ARCHIVE_PROGRESS_FORMAT_VERSION 2u
static inline void ps_ap_put32(unsigned char *p, uint32_t n)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char) (n >> (i * 8));
}
static inline uint32_t ps_ap_get32(const unsigned char *p)
{
	uint32_t n = 0;
	for (unsigned i = 0; i < 4; i++) n |= (uint32_t) p[i] << (i * 8);
	return n;
}
static inline void ps_ap_put64(unsigned char *p, uint64_t n)
{
	for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char) (n >> (i * 8));
}
static inline uint64_t ps_ap_get64(const unsigned char *p)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < 8; i++) n |= (uint64_t) p[i] << (i * 8);
	return n;
}
static inline uint32_t ps_ap_checksum(const unsigned char *p, unsigned n)
{
	uint32_t h = UINT32_C(2166136261);
	for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= UINT32_C(16777619); }
	return h;
}
static inline int ps_archive_progress_encode(const PsArchiveProgress *s,
											  unsigned char *out, unsigned n)
{
	if (!ps_archive_progress_persistable(s) || !out || n != PS_ARCHIVE_PROGRESS_FORMAT_BYTES)
		return -1;
	memset(out, 0, n);
	ps_ap_put32(out, PS_ARCHIVE_PROGRESS_FORMAT_MAGIC);
	ps_ap_put32(out + 4, PS_ARCHIVE_PROGRESS_FORMAT_VERSION);
	ps_ap_put32(out + 8, s->timeline);
	ps_ap_put64(out + 16, s->incarnation);
	ps_ap_put64(out + 24, s->attempt);
	ps_ap_put64(out + 32, s->marker);
	ps_ap_put64(out + 40, s->safe_lsn);
	ps_ap_put64(out + 48, s->target);
	ps_ap_put64(out + 56, s->grant_end);
	ps_ap_put64(out + 64, s->original_limit);
	ps_ap_put64(out + 72, s->max_extra);
	ps_ap_put64(out + 80, s->start_nonce_hi);
	ps_ap_put64(out + 88, s->start_nonce_lo);
	ps_ap_put32(out + 100, ps_ap_checksum(out, 100));
	return 0;
}
static inline int ps_archive_progress_decode(PsArchiveProgress *s,
											  const unsigned char *in, unsigned n)
{
	PsArchiveProgress x = {0};
	if (!s || !in || n != PS_ARCHIVE_PROGRESS_FORMAT_BYTES ||
		ps_ap_get32(in) != PS_ARCHIVE_PROGRESS_FORMAT_MAGIC ||
		ps_ap_get32(in + 4) != PS_ARCHIVE_PROGRESS_FORMAT_VERSION ||
		ps_ap_get32(in + 12) != 0 || ps_ap_get32(in + 96) != 0 ||
		ps_ap_get32(in + 100) != ps_ap_checksum(in, 100))
		return -1;
	x.timeline = ps_ap_get32(in + 8);
	x.incarnation = ps_ap_get64(in + 16);
	x.attempt = ps_ap_get64(in + 24);
	x.marker = ps_ap_get64(in + 32);
	x.safe_lsn = ps_ap_get64(in + 40);
	x.target = ps_ap_get64(in + 48);
	x.grant_end = ps_ap_get64(in + 56);
	x.original_limit = ps_ap_get64(in + 64);
	x.max_extra = ps_ap_get64(in + 72);
	x.start_nonce_hi = ps_ap_get64(in + 80);
	x.start_nonce_lo = ps_ap_get64(in + 88);
	if (!ps_archive_progress_persistable(&x))
		return -1;
	*s = x;
	return 0;
}
#endif
