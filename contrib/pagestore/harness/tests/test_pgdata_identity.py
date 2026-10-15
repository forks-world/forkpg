#!/usr/bin/env python3
"""Malformed build evidence must fail before loading or rewriting artifacts."""
import argparse
import copy
import json
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pagestore_pgdata_fixture as fixture

IDENTITY = dict(pg_control_version=1902, xlog_page_magic=53536,
                catalog_version_no=202606091)
INVALID = [None, [], {}, {"pg_control_version": 1902},
           {"xlog_page_magic": 53536}]
for key in fixture.PG_IDENTITY_GATE_KEYS:
    for value in [None, True, "1902", 1.5, 0, -1]:
        item = dict(IDENTITY)
        item[key] = value
        INVALID.append(item)


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = contextlib.redirect_stdout(io.StringIO())
        self.output.__enter__()
        self.addCleanup(self.output.__exit__, None, None, None)
        self.args = argparse.Namespace(require_build_match=True, build=None,
                                      format_tool=Path('unused'),
                                      postgres_payload_identity=Path('supplied'),
                                      postgres_payload_identity_tool=None)

    def check(self, recorded, build=IDENTITY, present=True, strict=True):
        metadata = {"role": "current", "capture_source": {
            "pagestore_sha": "a" * 40, "postgres_core_sha": "b" * 40,
            "postgres_major": 19, "cassert": True}}
        if present:
            metadata['pg_identity'] = recorded
        self.args.require_build_match = strict
        with patch.object(fixture, 'fixture_metadata', return_value=metadata), \
             patch.object(fixture, 'build_payload_identity', return_value=build):
            return fixture.check_one(self.args, self.root)

    def test_strict_check_requires_loaders_and_build_identity(self):
        for build, identity in [(None, Path('supplied')), (Path('build'), None)]:
            with self.subTest(build=build, identity=identity):
                self.args.build = build
                self.args.postgres_payload_identity = identity
                self.args.check = [self.root]
                with patch.object(fixture, 'check_one', return_value=0) as check_one, \
                     patch.object(fixture, 'fixture_metadata', return_value={'role': 'current'}):
                    with self.assertRaises(fixture.FixtureError):
                        fixture.check(self.args)
                    check_one.assert_not_called()

    def test_strict_check_requires_a_matching_current_fixture(self):
        self.args.build = Path('build')
        self.args.check = [self.root]
        for role, result in [('legacy', 0), ('current', None)]:
            with self.subTest(role=role), \
                 patch.object(fixture, 'check_one', return_value=result), \
                 patch.object(fixture, 'fixture_metadata', return_value={'role': role}):
                self.assertEqual(fixture.check(self.args), 1)

    def test_invalid_recorded_evidence_even_without_strict_gate(self):
        for value in INVALID:
            for strict in [False, True]:
                with self.subTest(value=value, strict=strict):
                    with self.assertRaises(fixture.FixtureError):
                        self.check(value, strict=strict)

    def test_invalid_checking_build_cannot_skip_foreign_fixture(self):
        for value in INVALID:
            with self.subTest(value=value):
                with self.assertRaises(fixture.FixtureError):
                    self.check(IDENTITY, build=value)

    def test_complete_foreign_identity_is_skipped(self):
        foreign = dict(IDENTITY, pg_control_version=1800, xlog_page_magic=53528)
        self.assertIsNone(self.check(foreign))

    def test_missing_identity_cannot_satisfy_strict_gate(self):
        self.assertEqual(self.check(None, present=False), 1)

    def test_legacy_missing_identity_keeps_non_strict_compatibility(self):
        (self.root / fixture.FORMAT_JSON).write_text("[]")
        with patch.object(fixture, "format_identities", return_value=[]):
            self.assertEqual(self.check(None, present=False, strict=False), 0)

    def test_same_major_catalog_change_still_loads(self):
        (self.root / fixture.FORMAT_JSON).write_text('[]')
        with patch.object(fixture, 'format_identities', return_value=[]):
            self.assertEqual(self.check(dict(IDENTITY, catalog_version_no=202606092)), 0)

    def test_recapture_rejection_preserves_every_file(self):
        source = self.root / 'source'
        source.mkdir()
        original = Path(fixture.__file__).parents[1] / 'fixtures' / 'pgdata-artifacts'
        metadata = json.loads((original / fixture.FIXTURE_JSON).read_text())
        (source / fixture.IDENTITY_JSON).write_text(json.dumps(metadata['identity']))
        for _, relative in fixture.ARTIFACTS.values():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'new artifacts')
        target = self.root / 'capture'
        target.mkdir()
        for name in [fixture.FIXTURE_JSON, fixture.FORMAT_JSON, fixture.ARTIFACTS_TAR]:
            (target / name).write_bytes(b'original')
        args = argparse.Namespace(source=source, capture=target, keep_pg_identity=True,
                                  postgres_payload_identity=None,
                                  postgres_payload_identity_tool=None)
        for keep in [False, True]:
            for value in INVALID:
                with self.subTest(keep=keep, value=value):
                    args.postgres_payload_identity = None if keep else Path("supplied")
                    previous = copy.deepcopy(metadata)
                    previous['pg_identity'] = value
                    (target / fixture.FIXTURE_JSON).write_text(json.dumps(previous))
                    before = {p.name: p.read_bytes() for p in target.iterdir()}
                    with patch.object(fixture, 'capture_source', return_value=None), \
                         patch.object(fixture, 'build_payload_identity',
                                      return_value=None if keep else value):
                        with self.assertRaises(fixture.FixtureError):
                            fixture.capture(args)
                    self.assertEqual(before, {p.name: p.read_bytes() for p in target.iterdir()})


if __name__ == '__main__':
    unittest.main()
