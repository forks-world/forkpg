/*-------------------------------------------------------------------------
 *
 * fuzz_crc_fixup.h
 *	  Structure-aware checksum recomputation for the persisted-format fuzz
 *	  targets (round 2, item 2).  See fuzz_crc_fixup.c for the per-format
 *	  rules and which targets have no checksum, or one this harness does
 *	  not fix up.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PS_FUZZ_CRC_FIXUP_H
#define PS_FUZZ_CRC_FIXUP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Recompute target_name's on-disk checksum field(s) over the (already
 * mutated) buffer, in place, following that format's real rule -- reusing
 * the product's own hash algorithms (all either PostgreSQL CRC-32C via
 * ps_crc32c_update()/PS_CRC32C_INIT/FIN in pagestore_artifact_format.h, or
 * the project's standard FNV-1a stepping function, replicated here byte-
 * for-byte from pagestore_core.c's fnv()/pagestore_manifest.c's
 * manifest_fnv1a()/etc. -- every persisted format in this codebase uses one
 * of these two, never a bespoke one) and the product's own struct layouts
 * (mirrored locally the way the existing pagestore_*_test.c files already
 * do for formats whose struct is private to one .c file, or included
 * directly where the type is public, e.g. PsKey, PsRetentionPin,
 * PsPruneFence). Checksums are recomputed for the mutated bytes. Where a
 * format has a required cross-file relationship, the fixed-up half also
 * synchronizes only the linked fields (for example a manifest identity/hash
 * with sibling payloads, or a watermark length with its sibling log). The
 * raw half skips this function, preserving malformed-field coverage.
 *
 * work_dir is the live store directory for this iteration. Some targets
 * need sibling reads or writes to keep cross-file checks valid: forkmeta
 * snapshot parts patch forkmeta_manifest_v1, the forkmeta manifest target
 * reads the pristine part templates, walidx_watermark reads its epoch log,
 * and walidx snapshot shard fixup updates its sibling manifest. Other
 * targets only touch buf/len.
 */
extern void ps_fuzz_crc_fixup(const char *target_name, const char *work_dir,
							   uint8_t *buf, size_t len);

#endif							/* PS_FUZZ_CRC_FIXUP_H */
