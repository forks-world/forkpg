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
        self.git("commit", "-q", "-m", "release tree copy")
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
        self.data = {"supported": [{"major": 18, "branch": "branchdb_18", "status": "candidate",
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
        (self.repo / "contrib/pagestore/example").write_text("changed release tree\n")
        self.git("add", "contrib")
        self.git("commit", "-q", "-m", "ci: changed release")
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
        with self.assertRaisesRegex(ReleaseBranchesError, "no PostgreSQL core/build"):
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

    def test_planned_entries_need_no_repository(self):
        self.data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        check_release_provenance(self.data, repo=self.repo / "nonexistent")


if __name__ == "__main__":
    unittest.main()
