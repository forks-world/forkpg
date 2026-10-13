"""Release promotions must correspond to real upstream/source/branch objects."""
import copy
from pathlib import Path
import subprocess
import tempfile
import unittest

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
        self.git("commit", "-q", "--allow-empty", "-m", "upstream base")
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
        self.source = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "-b", "branchdb_18", self.base)
        self.git("checkout", self.source, "--", "contrib/pagestore")
        self.git("commit", "-q", "-m", "release tree copy")
        self.origin = root / "origin.git"
        subprocess.run(["git", "clone", "--bare", "-q", str(self.repo), str(self.origin)], check=True)
        self.git("remote", "add", "origin", str(self.origin))
        self.data = {"supported": [{"major": 18, "branch": "branchdb_18", "status": "candidate",
                                   "base_tag": "REL_18_6", "contrib_sha": self.source}]}

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
        self.git("commit", "-q", "-m", "changed release")
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

    def test_planned_entries_need_no_repository(self):
        self.data["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        check_release_provenance(self.data, repo=self.repo / "nonexistent")


if __name__ == "__main__":
    unittest.main()
