"""Exercise release-branch rewrite guards in an isolated Git repository."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/branchdb-sync.sh"


class BranchSyncSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="branchdb-sync-test-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.name", "Sync test")
        self.git("config", "user.email", "sync-test@example.invalid")
        self.git("config", "commit.gpgSign", "false")
        self.git("config", "tag.gpgSign", "false")
        self.git("config", "core.hooksPath", "/dev/null")
        self.git("commit", "-q", "--allow-empty", "-m", "base")
        self.base = self.git("rev-parse", "HEAD")
        for tag in ("REL_18_6", "REL_17_6"):
            self.git("tag", tag)
        self.git("branch", "branchdb_18", self.base)
        self.git("branch", "branchdb_18-rc", self.base)
        self.git("commit", "-q", "--allow-empty", "-m", "new minor")
        self.git("tag", "REL_18_7")
        self.git("update-ref", "refs/remotes/upstream/REL_18_STABLE", "HEAD")
        self.git("update-ref", "refs/remotes/upstream/REL_19_STABLE", self.base)
        self.git("checkout", "-q", "-b", "branchdb_19", self.base)
        (self.repo / "feature").write_text("core feature\n")
        self.git("add", "feature")
        self.git("commit", "-q", "-m", "core feature\n\nBranchdb-Series: C1")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True,
                              capture_output=True, text=True, timeout=30).stdout.strip()

    def run_sync(self, *args):
        return subprocess.run(["bash", str(SCRIPT), *args], cwd=self.repo,
                              capture_output=True, text=True, timeout=30)

    def assert_refused_without_rewrite(self, *args):
        before = self.git("show-ref")
        head = self.git("rev-parse", "HEAD")
        result = self.run_sync(*args)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.git("show-ref"), before)
        self.assertEqual(self.git("rev-parse", "HEAD"), head)
        return result

    def test_minor_rejects_foreign_major_tag(self):
        result = self.assert_refused_without_rewrite("minor", "18", "REL_17_6")
        self.assertIn("REL_18_", result.stderr)

    def test_forward_rejects_foreign_major_tag(self):
        self.assert_refused_without_rewrite("forward", "branchdb_19", "branchdb_18", "REL_17_6")

    def test_minor_rejects_branch_named_like_a_release_tag(self):
        self.git("branch", "REL_18_fake", self.base)
        self.assert_refused_without_rewrite("minor", "18", "REL_18_fake")

    def test_forward_refuses_resume_on_different_minor_base(self):
        result = self.assert_refused_without_rewrite("forward", "branchdb_19", "branchdb_18", "REL_18_7")
        self.assertIn("differs from requested tag", result.stderr)

    def test_verify_rejects_unlanded_same_contrib_tree(self):
        self.git("update-ref", "refs/remotes/origin/pagestore", "branchdb_19")
        self.git("checkout", "-q", "branchdb_18-rc")
        self.git("commit", "-q", "--allow-empty", "-m", "unlanded source")
        result = self.assert_refused_without_rewrite("verify", "branchdb_18-rc", "HEAD")
        self.assertIn("not an ancestor of origin/pagestore", result.stderr)

    def test_verify_accepts_landed_source(self):
        self.git("tag", "-d", "REL_17_6")  # keep the PG18 base tag unambiguous
        self.git("update-ref", "refs/remotes/origin/pagestore", "branchdb_19")
        result = self.run_sync("verify", "branchdb_18-rc", self.base)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_sync_rejects_remote_tag_and_missing_targets(self):
        self.git("update-ref", "refs/remotes/origin/pagestore", "branchdb_19")
        self.git("update-ref", "refs/remotes/origin/branchdb_18", self.base)
        for target in ("origin/branchdb_18", "REL_18_6", "branchdb_20"):
            with self.subTest(target=target):
                result = self.assert_refused_without_rewrite(
                    "sync-contrib", target, "origin/pagestore")
                self.assertIn("existing local branch", result.stderr)
                self.assertEqual(self.git("symbolic-ref", "HEAD"),
                                 "refs/heads/branchdb_19")

    def test_sync_rejects_source_without_contrib_before_mutating(self):
        path = self.repo / "contrib/pagestore/example"
        path.parent.mkdir(parents=True)
        path.write_text("preserve me\n")
        self.git("add", "contrib/pagestore")
        self.git("commit", "-q", "-m", "source tree")
        self.git("update-ref", "refs/remotes/origin/pagestore", "HEAD")
        result = self.assert_refused_without_rewrite("sync-contrib", "branchdb_19", self.base)
        self.assertIn("no contrib/pagestore tree", result.stderr)
        self.assertEqual(path.read_text(), "preserve me\n")
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_verify_ignores_tag_shadowing_release_branch(self):
        self.git("tag", "-d", "REL_17_6")
        self.git("update-ref", "refs/remotes/origin/pagestore", self.base)
        self.git("tag", "branchdb_18-rc", self.base)
        self.git("checkout", "-q", "branchdb_18-rc")
        path = self.repo / "rogue"
        path.write_text("not a permitted release commit\n")
        self.git("add", "rogue")
        self.git("commit", "-q", "-m", "rogue change")
        result = self.run_sync("verify", "branchdb_18-rc", self.base)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("rogue change", result.stdout + result.stderr)
        self.git("update-ref", "refs/remotes/origin/branchdb_18-rc", "HEAD")
        self.git("checkout", "-q", "branchdb_19")
        self.git("branch", "-D", "branchdb_18-rc")
        result = self.run_sync("verify", "branchdb_18-rc", self.base)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("rogue change", result.stdout + result.stderr)

    def test_sync_records_immutable_oid_when_alias_moves(self):
        self.git("tag", "-d", "REL_17_6")
        path = self.repo / "contrib/pagestore/example"
        path.parent.mkdir(parents=True)
        path.write_text("first capture\n")
        self.git("add", "contrib/pagestore")
        self.git("commit", "-q", "-m", "source capture")
        source = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/origin/pagestore", source)
        result = self.run_sync("sync-contrib", "branchdb_18-rc", "origin/pagestore")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        message = self.git("log", "-1", "--format=%B")
        self.assertIn("pagestore@" + source, message)
        self.assertNotIn("pagestore@origin/pagestore", message)
        self.git("checkout", "-q", "branchdb_19")
        path.write_text("later capture\n")
        self.git("add", "contrib/pagestore")
        self.git("commit", "-q", "-m", "later source")
        self.git("update-ref", "refs/remotes/origin/pagestore", "HEAD")
        result = self.run_sync("verify", "branchdb_18-rc", source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("show", "branchdb_18-rc:contrib/pagestore/example"),
                         "first capture")

    def test_verify_resolves_source_alias_once(self):
        self.git("tag", "-d", "REL_17_6")
        self.git("update-ref", "refs/remotes/origin/pagestore", self.base)
        result = self.run_sync("verify", "branchdb_18-rc", "origin/pagestore")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(self.base, result.stdout)
        self.assertNotIn("pagestore@origin/pagestore", result.stdout)

    def test_forward_resumes_matching_base_and_skips_applied_series(self):
        result = self.run_sync("forward", "branchdb_19", "branchdb_18", "REL_18_6")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.repo / "feature").read_text(), "core feature\n")
        head = self.git("rev-parse", "branchdb_18-rc")
        result = self.run_sync("forward", "branchdb_19", "branchdb_18", "REL_18_6")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git("rev-parse", "branchdb_18-rc"), head)


if __name__ == "__main__":
    unittest.main()
