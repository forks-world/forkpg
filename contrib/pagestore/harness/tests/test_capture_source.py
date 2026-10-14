"""Fixture regeneration must not silently discard or reuse provenance."""
from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))
from pagestore_capture_source import CaptureSourceError, capture_source, validate_capture_source
import pagestore_fixture as store
import pagestore_pgdata_fixture as pgdata

SOURCE = {"pagestore_sha": "a" * 40, "postgres_core_sha": "b" * 40,
          "postgres_major": 18, "cassert": True}


class CaptureSourceTests(unittest.TestCase):
    def test_strict_matching_fixture_requires_provenance_in_both_checkers(self):
        for module in [store, pgdata]:
            for strict in [False, True]:
                with self.subTest(module=module.__name__, strict=strict), tempfile.TemporaryDirectory() as temp:
                    fixture = Path(temp)
                    (fixture / 'format.json').write_text('[]')
                    identity = dict(pg_control_version=1902, xlog_page_magic=53536)
                    metadata = dict(role='current', pg_identity=identity)
                    args = argparse.Namespace(require_build_match=strict, format_tool=Path('/unused'),
                                              build=None, only=['no-such-mutation'],
                                              postgres_payload_identity=Path('/supplied'),
                                              postgres_payload_identity_tool=None)
                    with patch.object(module, 'fixture_metadata', return_value=metadata), \
                         patch.object(module, 'format_identities', return_value=[]), \
                         patch.object(pgdata, 'build_payload_identity', return_value=identity), \
                         patch.object(store, 'check_reopen'), contextlib.redirect_stdout(io.StringIO()):
                        if strict:
                            with self.assertRaisesRegex(CaptureSourceError, 'needs capture_source'):
                                module.check_one(args, fixture)
                        else:
                            self.assertEqual(module.check_one(args, fixture), 0)
                        metadata['capture_source'] = dict(SOURCE, postgres_major=19)
                        self.assertEqual(module.check_one(args, fixture), 0)

    def test_strict_foreign_and_legacy_fixtures_do_not_require_capture_evidence(self):
        for module in [store, pgdata]:
            for role in ['legacy', 'current']:
                with self.subTest(module=module.__name__, role=role), tempfile.TemporaryDirectory() as temp:
                    fixture = Path(temp)
                    (fixture / 'format.json').write_text('[{"old": true}]')
                    metadata = dict(role=role, pg_identity=dict(pg_control_version=1800, xlog_page_magic=53528))
                    args = argparse.Namespace(require_build_match=True, format_tool=Path('/unused'), build=None,
                                              postgres_payload_identity=Path('/supplied'),
                                              postgres_payload_identity_tool=None)
                    with patch.object(module, 'fixture_metadata', return_value=metadata), \
                         patch.object(module, 'format_identities', return_value=[]), \
                         patch.object(pgdata, 'build_payload_identity', return_value=dict(
                             pg_control_version=1902, xlog_page_magic=53536)), \
                         patch.object(store, 'check_reopen', side_effect=(
                             store.ForeignPayload('foreign major') if role=='current' else None)), \
                         contextlib.redirect_stdout(io.StringIO()):
                        result = module.check_one(args, fixture)
                        self.assertEqual(result, None if role=='current' or module is pgdata else 0)

    def test_validation(self):
        self.assertEqual(validate_capture_source(SOURCE), SOURCE)
        invalid = [{}, dict(SOURCE, unexpected=1), dict(SOURCE, pagestore_sha="abc"),
                   dict(SOURCE, postgres_core_sha="z" * 40),
                   dict(SOURCE, postgres_major=True), dict(SOURCE, postgres_major=0),
                   dict(SOURCE, cassert="true")]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(CaptureSourceError):
                validate_capture_source(value)

    def test_explicit_source_replaces_old_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "fixture.json").write_text(json.dumps({"capture_source": SOURCE}))
            fresh = dict(SOURCE, pagestore_sha="c" * 40, cassert=False)
            source = root / "source.json"
            source.write_text(json.dumps(fresh))
            self.assertEqual(capture_source(argparse.Namespace(capture_source=source), root), fresh)
            with self.assertRaises(CaptureSourceError):
                capture_source(argparse.Namespace(capture_source=None), root)

    def test_legacy_capture_without_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "fixture.json").write_text('{}')
            self.assertIsNone(capture_source(argparse.Namespace(capture_source=None), root))

    def test_both_checkers_reject_malformed_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = Path(temp) / "fixture"
            fixture.mkdir()
            (fixture / "fixture.json").write_text(
                json.dumps({"capture_source": dict(SOURCE, cassert="true")}))
            for script, extra in [
                ("pagestore_fixture.py", ["--daemon-binary", "/missing",
                 "--client-binary", "/missing", "--inspect-binary", "/missing"]),
                ("pagestore_pgdata_fixture.py", []),
            ]:
                with self.subTest(script=script):
                    result = subprocess.run([sys.executable, str(HARNESS / script),
                                             "--check", str(fixture), "--format-tool", "/missing",
                                             *extra], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("cassert must be a JSON boolean", result.stderr)

    def test_both_capture_commands_refuse_before_touching_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = Path(temp) / "fixture"
            fixture.mkdir()
            metadata = json.dumps({"capture_source": SOURCE})
            (fixture / "fixture.json").write_text(metadata)
            for script, archive, extra in [
                ("pagestore_fixture.py", "store.tar.gz",
                 ["--daemon-binary", "/missing", "--client-binary", "/missing",
                  "--inspect-binary", "/missing"]),
                ("pagestore_pgdata_fixture.py", "artifacts.tar.gz", ["--source", "/missing"]),
            ]:
                with self.subTest(script=script):
                    (fixture / archive).write_bytes(b"existing archive")
                    result = subprocess.run([sys.executable, str(HARNESS / script),
                                             "--capture", str(fixture), "--format-tool", "/missing",
                                             *extra], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("needs --capture-source", result.stderr)
                    self.assertEqual((fixture / archive).read_bytes(), b"existing archive")
                    self.assertEqual((fixture / "fixture.json").read_text(), metadata)


if __name__ == "__main__":
    unittest.main()
