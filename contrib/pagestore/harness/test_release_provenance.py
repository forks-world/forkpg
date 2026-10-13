"""Release promotions must correspond to real upstream/source/branch objects."""
import copy
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import pagestore_release_branches as manifest_reader

from pagestore_release_branches import check_release_provenance, ReleaseBranchesError


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="release-provenance-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.repo = root / "work"
        self.repo.mkdir()
        self.git("init", "-q")
        for key, value in (("user.name", "Test"), ("user.email", "test@example.invalid"),
                           ("commit.gpgSign", "false"), ("tag.gpgSign", "false"),
                           ("core.hooksPath", "/dev/null")):
            self.git("config", key, value)
        (self.repo / "configure.ac").write_text("AC_INIT([PostgreSQL], [18.6])\n")
        self.git("add", "configure.ac")
        self.git("commit", "-q", "-m", "upstream base")
        self.base = self.git("rev-parse", "HEAD")
        self.git("tag", "REL_18_6")
        self.git("checkout", "-q", "-b", "REL_18_STABLE")
        self.git("commit", "-q", "--allow-empty", "-m", "next upstream minor")
        self.git("tag", "REL_18_7")
        self.upstream = root / "upstream.git"
        subprocess.run(["git", "clone", "--bare", "-q", str(self.repo), str(self.upstream)], check=True)
        self.git("checkout", "-q", "-b", "pagestore", self.base)
        path = self.repo / "contrib/pagestore/example"
        path.parent.mkdir(parents=True)
        path.write_text("source tree\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "pagestore source")
        self.source_commits = {}
        for label in range(1, 8):
            path = self.repo / f"src/core{label}"
            path.parent.mkdir(exist_ok=True)
            path.write_text(f"core {label}\n")
            self.git("add", str(path.relative_to(self.repo)))
            self.git("commit", "-q", "-m", f"development core {label}")
            self.source_commits[f"C{label}"] = self.git("rev-parse", "HEAD")
        self.source = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "-b", "branchdb_18", self.base)
        self.git("checkout", self.source, "--", "contrib/pagestore")
        self.git("commit", "-q", "-m", f"contrib/pagestore: sync to pagestore@{self.source}")
        self.core_series = {}
        for label, source in self.source_commits.items():
            self.git("cherry-pick", "-x", source)
            self.git("commit", "-q", "--amend", "-m",
                     f"ported {label}\n\nBranchdb-Series: {label}\n(cherry picked from commit {source})")
            self.core_series[label] = {"commit": self.git("rev-parse", "HEAD"), "sources": [source]}
        self.candidate = self.git("rev-parse", "HEAD")
        self.origin = root / "origin.git"
        subprocess.run(["git", "clone", "--bare", "-q", str(self.repo), str(self.origin)], check=True)
        self.git("remote", "add", "origin", str(self.origin))
        self.data = {"schema": 1, "unsupported": [], "supported": [{"major": 18, "branch": "branchdb_18", "status": "candidate",
                                   "base_tag": "REL_18_6", "contrib_sha": self.source,
                                   "core_series": self.core_series}]}

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, text=True,
                              capture_output=True, check=True, timeout=30).stdout.strip()

    def check(self, data=None):
        check_release_provenance(data or self.data, repo=self.repo, upstream=str(self.upstream))

    def test_matching_provenance_and_annotated_tag(self):
        self.check()
        self.git("tag", "-a", "REL_18_6", "-f", "-m", "annotated upstream release", self.base)
        self.git("push", "--force", str(self.upstream), "refs/tags/REL_18_6")
        self.check()

    def test_missing_objects_or_wrong_base_are_rejected(self):
        for field, value in (("base_tag", "REL_18_999"), ("base_tag", "REL_18_7"),
                             ("contrib_sha", "0" * 40)):
            with self.subTest(field=field, value=value):
                data = copy.deepcopy(self.data)
                data["supported"][0][field] = value
                with self.assertRaises(ReleaseBranchesError):
                    self.check(data)

    def test_unlanded_source_is_rejected(self):
        self.git("commit", "-q", "--allow-empty", "-m", "unlanded same tree")
        self.data["supported"][0]["contrib_sha"] = self.git("rev-parse", "HEAD")
        with self.assertRaises(ReleaseBranchesError):
            self.check()

    def test_stale_source_tree_is_rejected(self):
        self.git("checkout", "-q", "pagestore")
        (self.repo / "contrib/pagestore/example").write_text("new source tree\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "new source capture")
        newer = self.git("rev-parse", "HEAD")
        self.git("push", "origin", "pagestore")
        self.git("checkout", "-q", "branchdb_18")
        self.git("checkout", newer, "--", "contrib/pagestore")
        self.git("commit", "-q", "-m", f"contrib/pagestore: sync to pagestore@{newer}")
        self.git("push", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "contrib tree differs"):
            self.check()

    def test_shallow_checkout_gets_complete_history(self):
        shallow = Path(self.temp.name) / "shallow"
        subprocess.run(["git", "clone", "--depth", "1", "--branch", "pagestore", "-q",
                        self.origin.as_uri(), str(shallow)], check=True, timeout=30)
        check_release_provenance(self.data, repo=shallow, upstream=str(self.upstream))
        result = subprocess.run(["git", "rev-parse", "--is-shallow-repository"],
                                cwd=shallow, capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), "false")

    def test_source_without_pagestore_and_missing_branch_are_rejected(self):
        self.data["supported"][0]["contrib_sha"] = self.base
        with self.assertRaises(ReleaseBranchesError):
            self.check()
        self.data["supported"][0]["contrib_sha"] = self.source
        subprocess.run(["git", "--git-dir", str(self.origin), "update-ref", "-d",
                        "refs/heads/branchdb_18"], check=True)
        with self.assertRaises(ReleaseBranchesError):
            self.check()

    def test_release_shape_is_enforced(self):
        for message in ("untrailered core edit", "duplicate\n\nBranchdb-Series: C7",
                        "malformed\n\nBranchdb-Series: C7\nBranchdb-Series: C8"):
            with self.subTest(message=message):
                self.git("reset", "--hard", self.candidate)
                (self.repo / "unrelated").write_text("extra core change\n")
                self.git("add", "unrelated")
                self.git("commit", "-q", "-m", message)
                self.git("push", "--force", "origin", "branchdb_18")
                with self.assertRaises(ReleaseBranchesError):
                    self.check()
        self.git("reset", "--hard", self.candidate + "~1")
        self.git("push", "--force", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "exactly one C1-C7"):
            self.check()

    def test_merge_commits_are_rejected(self):
        self.git("checkout", "-q", "-b", "side")
        (self.repo / "side").write_text("side\n")
        self.git("add", "side")
        self.git("commit", "-q", "-m", "ci: side")
        self.git("checkout", "-q", "branchdb_18")
        self.git("merge", "--no-ff", "side", "-m", "ci: merge")
        self.git("push", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "merge commits"):
            self.check()

    def test_authoritative_manifest_and_frozen_candidate(self):
        self.git("checkout", "-q", "pagestore")
        path = self.repo / "contrib/pagestore/release-branches.json"
        data = {"schema": 1, "unsupported": [], **self.data}
        path.write_text(json.dumps(data))
        self.git("add", str(path.relative_to(self.repo)))
        self.git("commit", "-q", "-m", "record authoritative promotion")
        self.git("push", "origin", "pagestore")
        # A stale local manifest must not shadow the authoritative copy.
        local = copy.deepcopy(data)
        local["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        path.write_text(json.dumps(local))
        self.assertEqual(manifest_reader.load_authoritative(
            candidate_sha=self.candidate, repo=self.repo, upstream=str(self.upstream))["supported"], data["supported"])
        # Moving the branch must not change what the frozen tag validates.
        path.write_text(json.dumps(data))
        self.git("checkout", "-q", "branchdb_18")
        (self.repo / "contrib/pagestore/example").write_text("later branch tree\n")
        self.git("add", "contrib/pagestore/example")
        self.git("commit", "-q", "-m", "ci: later branch change")
        self.git("push", "origin", "branchdb_18")
        manifest_reader.check_release_provenance(self.data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=self.candidate)
        with self.assertRaises(ReleaseBranchesError):
            self.check()
        later = self.git("rev-parse", "HEAD")
        with self.assertRaises(ReleaseBranchesError):
            manifest_reader.load_authoritative(candidate_sha=later, repo=self.repo,
                                               upstream=str(self.upstream))

    def test_planned_authoritative_entry_cannot_accept_candidate(self):
        self.data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        with self.assertRaisesRegex(ReleaseBranchesError, "no promoted authoritative"):
            manifest_reader.check_release_provenance(self.data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=self.candidate)

    def test_field_lookup_is_offline(self):
        path = Path(self.temp.name) / "manifest.json"
        path.write_text(json.dumps({"schema": 1, "unsupported": [], **self.data}))
        output = io.StringIO()
        with mock.patch.object(manifest_reader, "DEFAULT_PATH", path), \
             mock.patch.object(manifest_reader, "check_release_provenance",
                               side_effect=AssertionError("network must not run")) as check, \
             contextlib.redirect_stdout(output):
            self.assertEqual(manifest_reader._print_field(18, "contrib_sha"), 0)
            check.assert_not_called()
        self.assertEqual(output.getvalue().strip(), self.source)

    def test_candidate_requires_release_lineage(self):
        self.git("checkout", "-q", "-b", "unrelated")
        (self.repo / "unrelated").write_text("allowed ancillary commit\n")
        self.git("add", "unrelated")
        self.git("commit", "-q", "-m", "ci: unrelated candidate")
        candidate = self.git("rev-parse", "HEAD")
        with self.assertRaises(ReleaseBranchesError):
            manifest_reader.check_release_provenance(self.data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=candidate)
        # The same commit is valid PR evidence, but not a frozen release tag.
        manifest_reader.check_release_provenance(self.data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=candidate, require_lineage=False)

    def test_empty_core_and_unlanded_core_source_are_rejected(self):
        self.git("reset", "--hard", self.candidate + "~1")
        self.git("commit", "-q", "--allow-empty", "-m", "empty\n\nBranchdb-Series: C7")
        self.data["supported"][0]["core_series"]["C7"]["commit"] = self.git("rev-parse", "HEAD")
        self.git("push", "--force", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "no PostgreSQL core/build"):
            self.check()
        self.git("reset", "--hard", self.candidate)
        self.git("push", "--force", "origin", "branchdb_18")
        self.data["supported"][0]["core_series"]["C7"]["commit"] = self.candidate
        self.data["supported"][0]["core_series"]["C7"]["sources"] = ["0" * 40]
        with self.assertRaises(ReleaseBranchesError):
            self.check()

    def test_contrib_only_core_label_is_rejected(self):
        self.git("reset", "--hard", self.candidate + "~1")
        (self.repo / "contrib/pagestore/example").write_text("not a core patch\n")
        self.git("add", "contrib/pagestore/example")
        self.git("commit", "-q", "-m", "contrib only\n\nBranchdb-Series: C7")
        self.data["supported"][0]["core_series"]["C7"]["commit"] = self.git("rev-parse", "HEAD")
        self.git("push", "--force", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "changes contrib/pagestore"):
            self.check()

    def test_checked_branch_revision_is_returned(self):
        revisions = manifest_reader.check_release_provenance(self.data, repo=self.repo,
            upstream=str(self.upstream))
        self.assertEqual(revisions, {"branchdb_18": self.candidate})
        (self.repo / "extra").write_text("new ancillary commit\n")
        self.git("add", "extra")
        self.git("commit", "-q", "-m", "ci: later branch")
        self.git("push", "origin", "branchdb_18")
        self.assertEqual(revisions["branchdb_18"], self.candidate)

    def test_first_planned_sync_can_be_reviewed_but_not_accepted(self):
        data = copy.deepcopy(self.data)
        data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None, core_series=None)
        revisions = manifest_reader.check_release_provenance(data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=self.candidate, require_lineage=False)
        self.assertEqual(revisions, {"branchdb_18": self.candidate})
        with self.assertRaises(ReleaseBranchesError):
            manifest_reader.check_release_provenance(data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=self.candidate)
        self.assertIsNone(data["supported"][0]["contrib_sha"])
        subprocess.run(["git", "--git-dir", str(self.origin), "update-ref", "-d",
                        "refs/heads/branchdb_18"], check=True)
        self.assertEqual(manifest_reader.check_release_provenance(data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=self.candidate, require_lineage=False),
            {"branchdb_18": self.candidate})

    def test_unsupported_reference_skips_only_pr_validation(self):
        (self.repo / "configure.ac").write_text("AC_INIT([PostgreSQL], [13.23])\n")
        self.git("add", "configure.ac")
        self.git("commit", "-q", "-m", "ci: synthetic PG13 reference")
        sha = self.git("rev-parse", "HEAD")
        data = copy.deepcopy(self.data)
        data["unsupported"] = [{"major": 13, "branch": "branchdb_13", "reason": "EOL"}]
        self.assertEqual(manifest_reader.check_release_provenance(data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=sha, require_lineage=False), {})
        with self.assertRaises(ReleaseBranchesError):
            manifest_reader.check_release_provenance(data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=sha)

    def test_planned_port_accepts_documented_short_source_commit(self):
        source = self.source_commits["C7"]
        self.git("commit", "-q", "--amend", "-m",
                 f"build port\n\nSource commit: {source[:11]}.\n\nBranchdb-Series: C7")
        sha = self.git("rev-parse", "HEAD")
        data = copy.deepcopy(self.data)
        data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None, core_series=None)
        self.assertEqual(manifest_reader.check_release_provenance(data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=sha, require_lineage=False), {"branchdb_18": sha})

    def test_core_squash_may_include_landed_documentation_sources(self):
        self.git("checkout", "-q", "pagestore")
        (self.repo / "port-notes").write_text("source documentation\n")
        self.git("add", "port-notes")
        self.git("commit", "-q", "-m", "port documentation")
        doc = self.git("rev-parse", "HEAD")
        self.git("push", "origin", "pagestore")
        self.git("checkout", "-q", "branchdb_18")
        source = self.source_commits["C7"]
        self.git("commit", "-q", "--amend", "-m",
                 f"build port\n\nSource commits: {source}, {doc}.\n\nBranchdb-Series: C7")
        record = self.data["supported"][0]["core_series"]["C7"]
        record.update(commit=self.git("rev-parse", "HEAD"), sources=[source, doc])
        self.git("push", "--force", "origin", "branchdb_18")
        self.check()
        record["sources"] = [doc]
        with self.assertRaisesRegex(ReleaseBranchesError, "sources have no core/build"):
            self.check()

    def test_promoted_minor_update_is_reviewable_before_metadata_update(self):
        previous = copy.deepcopy(self.data)
        self.git("rebase", "--onto", "REL_18_7", self.base, "branchdb_18")
        candidate = self.git("rev-parse", "HEAD")
        self.assertEqual(manifest_reader.check_release_provenance(self.data, repo=self.repo,
            upstream=str(self.upstream), candidate_sha=candidate, require_lineage=False),
            {"branchdb_18": candidate})
        self.assertEqual(self.data, previous)
        with self.assertRaises(ReleaseBranchesError):
            manifest_reader.check_release_provenance(self.data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=candidate)

    def test_wrong_target_major_is_rejected(self):
        with self.assertRaisesRegex(ReleaseBranchesError, "does not match target PG17"):
            manifest_reader.check_release_provenance(self.data, repo=self.repo,
                upstream=str(self.upstream), candidate_sha=self.candidate,
                require_lineage=False, expected_major=17)

    def test_direct_contrib_edit_then_restore_is_rejected(self):
        path = self.repo / "contrib/pagestore/example"
        path.write_text("direct edit\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "direct contrib edit")
        path.write_text("source tree\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "restore source tree")
        self.git("push", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "not a mechanical sync"):
            self.check()

    def test_falsely_claimed_mechanical_sync_is_rejected(self):
        path = self.repo / "contrib/pagestore/example"
        path.write_text("wrong claimed source\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", f"contrib/pagestore: sync to pagestore@{self.source}")
        path.write_text("source tree\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", f"contrib/pagestore: sync to pagestore@{self.source}")
        self.git("push", "origin", "branchdb_18")
        with self.assertRaisesRegex(ReleaseBranchesError, "differs from cited source"):
            self.check()

    def test_authoritative_checker_ignores_proposed_helper_stub(self):
        # Keep the production authority URL in the checker while mapping it
        # to this test's isolated authoritative Git repository.
        self.git("config", f"url.{self.upstream.as_uri()}.insteadOf",
                 "https://github.com/postgres/postgres.git")
        self.git("checkout", "-q", "pagestore")
        harness = self.repo / "contrib/pagestore/harness"
        harness.mkdir()
        source = Path(manifest_reader.__file__).read_text()
        (harness / "pagestore_release_branches.py").write_text(source)
        (harness / "test_release_provenance.py").write_text(Path(__file__).read_text())
        (self.repo / "contrib/pagestore/release-branches.json").write_text(json.dumps(self.data))
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "authoritative checker and policy")
        self.git("push", "origin", "pagestore")
        trusted_revision = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "branchdb_18")
        harness.mkdir(exist_ok=True)
        (harness / "pagestore_release_branches.py").write_text("print('STUB RAN')\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "stub the validator")
        candidate = self.git("rev-parse", "HEAD")
        # Execute the actual workflow extraction and validation programs.
        import os
        import textwrap
        workflow = Path(__file__).resolve().parents[3] / ".github/workflows/pagestore-release-provenance.yml"
        if not workflow.exists():
            # When the trusted regression file itself runs from an extracted
            # cache, test the same git-show/bootstrap contract directly.
            extract = 'mkdir -p "$RUNNER_TEMP/pagestore-trusted-checker"\ngit show "$TRUSTED_REVISION:contrib/pagestore/harness/pagestore_release_branches.py" > "$RUNNER_TEMP/pagestore-trusted-checker/pagestore_release_branches.py"'
            validate = 'python3 "$RUNNER_TEMP/pagestore-trusted-checker/pagestore_release_branches.py" --repository "$GITHUB_WORKSPACE" --authoritative-manifest --release-pr-sha "$RELEASE_SHA" --expected-major "$EXPECTED_MAJOR"'
        else:
            def program(name):
                section = workflow.read_text().split(f"      - name: {name}\n", 1)[1]
                section = section.split("\n      - name:", 1)[0]
                return textwrap.dedent(section.split("        run: |\n", 1)[1])
            extract = program("Extract authoritative validation code")
            validate = program("Validate checked-out release commit")
        env = {**os.environ, "RUNNER_TEMP": self.temp.name, "TRUSTED_REVISION": trusted_revision,
               "GITHUB_WORKSPACE": str(self.repo), "RELEASE_SHA": candidate, "EXPECTED_MAJOR": "18"}
        subprocess.run(["bash", "-e", "-c", extract], cwd=self.repo, env=env, check=True, timeout=30)
        result = subprocess.run(["bash", "-e", "-c", validate], cwd=self.repo, env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("STUB RAN", result.stdout + result.stderr)
        self.assertIn("not a mechanical sync", result.stderr)

    def test_planned_entries_need_no_repository(self):
        self.data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        check_release_provenance(self.data, repo=self.repo / "nonexistent")


if __name__ == "__main__":
    unittest.main()
