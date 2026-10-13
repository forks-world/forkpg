#!/usr/bin/env python3
"""Reader for ``contrib/pagestore/release-branches.json``: the supported-major
manifest.

The manifest is the single place that says which PostgreSQL majors
``branchdb_<N>`` supports (``supported``, each with a ``status`` of
``planned``, ``candidate``, ``released`` or ``released-preview``, and the
``base_tag``/``contrib_sha`` it was last synced at -- both ``null`` until a
sync happens) and which are kept only for reference (``unsupported``, each
with a human-readable ``reason``, e.g. an upstream EOL date).

The manifest is **hand-maintained**: nothing writes ``base_tag``/
``contrib_sha`` automatically. ``scripts/branchdb-sync.sh sync-contrib``
and ``minor`` only *read* them (for ``status``'s "does contrib match the
recorded SHA" report); a release-branch PR that performs a sync updates
the manifest itself, on ``pagestore``, as part of that PR -- a branch's
own byte-identical ``contrib/pagestore`` copy has no way to record its own
commit SHA from inside itself.

Consumers:

* ``scripts/branchdb-sync.sh status`` invokes this module directly (as a
  subprocess, so the shell script stays dependency-free) to look up a
  major's recorded ``contrib_sha``.
* The nightly/release-acceptance workflows (V4 plan, P4) read
  ``candidate_revisions()`` to use verified commit snapshots for every branch
  that currently has release evidence riding on it, instead of a hardcoded
  YAML list.

Run this file directly (no arguments) for a self-test: it validates the
manifest's structure and promoted entries' Git provenance (schema, no duplicate or misnamed majors, every
supported major has a status this reader recognises, 13 and 14 are exactly
the unsupported set) and prints a one-line summary. Run it with
``--major N --field FIELD`` to print one manifest field for major N (the
lookup ``scripts/branchdb-sync.sh status`` uses).
"""
from __future__ import annotations

import argparse
import copy
import json
import re
import sys
import subprocess
import tempfile
from pathlib import Path
from typing import Any

DEFAULT_PATH = Path(__file__).resolve().parent.parent / "release-branches.json"
REQUIRED_SCHEMA = 1
VALID_STATUSES = {"planned", "candidate", "released", "released-preview"}
CANDIDATE_STATUSES = {"candidate", "released", "released-preview"}


class ReleaseBranchesError(Exception):
    pass


def _check_common(path: Path, entry: dict[str, Any], seen_majors: set[int]) -> None:
    major = entry.get("major")
    branch = entry.get("branch")
    if not isinstance(major, int):
        raise ReleaseBranchesError(f"{path}: entry {entry!r} has no integer 'major'")
    if major in seen_majors:
        raise ReleaseBranchesError(f"{path}: major {major} is listed more than once")
    seen_majors.add(major)
    if branch != f"branchdb_{major}":
        raise ReleaseBranchesError(
            f"{path}: major {major}'s branch is {branch!r}, expected 'branchdb_{major}'"
        )


def load(path: Path | None = None, *, verify_provenance: bool = True) -> dict[str, Any]:
    """Parse and validate release-branches.json.  Raises
    ReleaseBranchesError on any structural problem, so a workflow fans its
    matrix out from a manifest whose promoted entries match authoritative
    upstream tags and origin release trees. Promoted entries require network
    access; verification fails closed when authoritative refs are unavailable.
    verify_provenance=False is only for isolated structural validation tests."""
    path = path or DEFAULT_PATH
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ReleaseBranchesError(f"cannot read {path}: {error}") from error
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as error:
        raise ReleaseBranchesError(f"{path} is not valid JSON: {error}") from error

    if data.get("schema") != REQUIRED_SCHEMA:
        raise ReleaseBranchesError(
            f"{path}: schema must be {REQUIRED_SCHEMA}, got {data.get('schema')!r}"
        )

    supported = data.get("supported")
    unsupported = data.get("unsupported")
    if not isinstance(supported, list) or not supported:
        raise ReleaseBranchesError(f"{path}: 'supported' must be a non-empty list")
    if not isinstance(unsupported, list):
        raise ReleaseBranchesError(f"{path}: 'unsupported' must be a list")

    seen_majors: set[int] = set()
    for entry in supported:
        _check_common(path, entry, seen_majors)
        if entry.get("status") not in VALID_STATUSES:
            raise ReleaseBranchesError(
                f"{path}: supported major {entry.get('major')} has an unknown "
                f"status {entry.get('status')!r} (expected one of {sorted(VALID_STATUSES)})"
            )
        for field in ("base_tag", "contrib_sha"):
            if field not in entry:
                raise ReleaseBranchesError(
                    f"{path}: supported major {entry.get('major')} is missing '{field}' "
                    "(null is fine before it is synced; the key must still be present)"
                )
        if entry["status"] in CANDIDATE_STATUSES:
            major = entry["major"]
            tag = entry["base_tag"]
            sha = entry["contrib_sha"]
            if not isinstance(tag, str) or not re.fullmatch(
                    rf"REL_{major}_(?:[0-9]+(?:_[0-9]+)*|BETA[1-9][0-9]*|RC[1-9][0-9]*)", tag):
                raise ReleaseBranchesError(
                    f"{path}: {entry['status']} major {major} requires a matching upstream base_tag")
            if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
                raise ReleaseBranchesError(
                    f"{path}: {entry['status']} major {major} requires a full contrib_sha commit ID")
            series = entry.get("core_series")
            if not isinstance(series, dict) or set(series) != {f"C{n}" for n in range(1, 8)}:
                raise ReleaseBranchesError(f"{path}: promoted major {major} requires approved core_series C1-C7")
            for label, record in series.items():
                if (not isinstance(record, dict) or
                    not isinstance(record.get("commit"), str) or
                    not re.fullmatch(r"[0-9a-fA-F]{40}", record["commit"]) or
                    not isinstance(record.get("sources"), list) or not record["sources"] or
                    any(not isinstance(sha, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", sha)
                        for sha in record["sources"])):
                    raise ReleaseBranchesError(f"{path}: invalid approved {label} commit/source records")
    for entry in unsupported:
        _check_common(path, entry, seen_majors)
        if not isinstance(entry.get("reason"), str) or not entry["reason"]:
            raise ReleaseBranchesError(
                f"{path}: unsupported major {entry.get('major')} is missing a 'reason'"
            )

    if verify_provenance:
        data["_verified_revisions"] = check_release_provenance(data)
    return data


def _git(repo: Path, *args: str) -> str:
    try:
        result = subprocess.run(["git", *args], cwd=repo, capture_output=True,
                                text=True, check=True, timeout=120)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as error:
        raise ReleaseBranchesError(f"cannot verify release provenance: git {' '.join(args)}: {error}") from error
    return result.stdout.strip()


def load_authoritative(*, candidate_sha: str | None = None, repo: Path | None = None,
                       upstream: str = "https://github.com/postgres/postgres.git",
                       require_lineage: bool = True) -> dict[str, Any]:
    repo = repo or DEFAULT_PATH.parents[2]
    ref = "refs/pagestore-release-check/manifest"
    _git(repo, "fetch", "--no-tags", "origin", f"+refs/heads/pagestore:{ref}")
    manifest = _git(repo, "show", f"{ref}:contrib/pagestore/release-branches.json")
    with tempfile.TemporaryDirectory(prefix="pagestore-authoritative-manifest-") as tmp:
        path = Path(tmp) / "release-branches.json"
        path.write_text(manifest, encoding="utf-8")
        data = load(path, verify_provenance=False)
    data["_verified_revisions"] = check_release_provenance(data, repo=repo, upstream=upstream, candidate_sha=candidate_sha, require_lineage=require_lineage)
    return data


def staged_release_entry(entry: dict[str, Any], candidate: str, repo: Path,
                         upstream: str) -> dict[str, Any]:
    """Check a planned sync for PR review without claiming release promotion.

    Provenance comes from the mechanical sync and cited core sources. Reviewed
    port pins are recorded on pagestore only after this release PR lands.
    """
    major = entry["major"]
    stable = f"refs/pagestore-release-check/{major}/staged-stable"
    _git(repo, "fetch", "--no-tags", upstream, f"+refs/heads/REL_{major}_STABLE:{stable}")
    base = _git(repo, "merge-base", candidate, stable)
    advertised = _git(repo, "ls-remote", "--tags", upstream, f"refs/tags/REL_{major}_*")
    tags: dict[str, dict[str, str]] = {}
    for line in advertised.splitlines():
        oid, ref = line.split()
        tag = ref.removeprefix("refs/tags/").removesuffix("^{}")
        if re.fullmatch(rf"REL_{major}_(?:[0-9]+|BETA[1-9][0-9]*|RC[1-9][0-9]*)", tag):
            tags.setdefault(tag, {})["commit" if ref.endswith("^{}") else "object"] = oid
    matching = [tag for tag, objects in tags.items()
                if objects.get("commit", objects.get("object")) == base]
    if not matching:
        raise ReleaseBranchesError("planned release base has no authoritative upstream release tag")
    staged = copy.deepcopy(entry)
    staged["base_tag"] = sorted(matching)[0]
    messages = _git(repo, "log", "--format=%s", f"{base}..{candidate}")
    source = re.search(r"^contrib/pagestore: sync to pagestore@([0-9a-f]{40})$", messages, re.M)
    if source is None:
        raise ReleaseBranchesError("planned release needs a mechanical sync with a full pagestore source SHA")
    staged["contrib_sha"] = source.group(1)
    staged["core_series"] = {}
    for commit in _git(repo, "rev-list", f"{base}..{candidate}").splitlines():
        message = _git(repo, "show", "-s", "--format=%B", commit)
        labels = re.findall(r"^Branchdb-Series: (C[1-7])$", message, re.M)
        if not labels:
            continue
        if len(labels) != 1 or labels[0] in staged["core_series"]:
            raise ReleaseBranchesError("planned release has duplicate core series labels")
        citations = re.findall(r"\(cherry picked from commit ([0-9a-f]{40})\)", message)
        section = re.search(r"Source commits?[^:]*:\s*(.*?)(?:\n\s*\n|$)", message, re.S)
        if section:
            citations += re.findall(r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])", section.group(1))
        if not citations:
            raise ReleaseBranchesError(f"planned {labels[0]} port {commit} has no source provenance")
        sources = sorted({_git(repo, "rev-parse", "--verify", f"{sha}^{{commit}}") for sha in citations})
        staged["core_series"][labels[0]] = {"commit": commit, "sources": sources}
    return staged


def check_release_provenance(data: dict[str, Any], *, repo: Path | None = None,
                             upstream: str = "https://github.com/postgres/postgres.git",
                             candidate_sha: str | None = None,
                             require_lineage: bool = True) -> dict[str, str]:
    """Confirm promoted entries against freshly fetched authoritative Git refs.

    Fetches update only dedicated verification refs, never local branches/tags.
    Planned entries require no Git repository or network access.
    """
    repo = repo or DEFAULT_PATH.parents[2]
    entries = [e for e in data["supported"] if e["status"] in CANDIDATE_STATUSES]
    if candidate_sha is not None:
        if not re.fullmatch(r"[0-9a-fA-F]{40}", candidate_sha):
            raise ReleaseBranchesError("candidate SHA must be a full commit ID")
        configure = _git(repo, "show", f"{candidate_sha}:configure.ac")
        version = re.search(r"AC_INIT\(\[PostgreSQL\],\s*\[([0-9]+)", configure)
        if version is None:
            raise ReleaseBranchesError("candidate has no PostgreSQL major identity")
        major = int(version.group(1))
        if not require_lineage and major in unsupported_majors(data):
            print(f"skip - PG{major} is an unsupported reference branch without release evidence")
            return {}
        if not require_lineage:
            entries = [e for e in data["supported"] if e["major"] == major]
        else:
            entries = [e for e in entries if e["major"] == major]
        if not entries:
            raise ReleaseBranchesError(f"candidate PG{major} has no promoted authoritative manifest entry")
    if not entries:
        return {}
    repo = repo or DEFAULT_PATH.parents[2]

    def git(*args: str) -> str:
        return _git(repo, *args)

    main_ref = "refs/pagestore-release-check/pagestore"
    git("fetch", "--no-tags", "origin", f"+refs/heads/pagestore:{main_ref}")
    # A shallow checkout cannot prove ancestry/base identity. Obtain complete
    # history before making a promotion decision.
    if git("rev-parse", "--is-shallow-repository") == "true":
        git("fetch", "--unshallow", "--no-tags", "origin", f"+refs/heads/pagestore:{main_ref}")
    main_ref = git("rev-parse", f"{main_ref}^{{commit}}")
    revisions: dict[str, str] = {}
    for entry in entries:
        if entry["status"] == "planned":
            if candidate_sha is None or require_lineage:
                raise ReleaseBranchesError("planned entries cannot qualify release candidates")
            entry = staged_release_entry(entry, candidate_sha, repo, upstream)
        major, branch, tag, source = (entry[k] for k in ("major", "branch", "base_tag", "contrib_sha"))
        prefix = f"refs/pagestore-release-check/{major}"
        tag_ref, stable_ref, branch_ref = (prefix + suffix for suffix in ("/tag", "/stable", "/branch"))
        git("fetch", "--no-tags", upstream, f"+refs/tags/{tag}:{tag_ref}",
            f"+refs/heads/REL_{major}_STABLE:{stable_ref}")
        if candidate_sha is None or require_lineage:
            git("fetch", "--no-tags", "origin", f"+refs/heads/{branch}:{branch_ref}")
            release_head = git("rev-parse", f"{branch_ref}^{{commit}}")
            branch_ref = release_head
        stable_ref = git("rev-parse", f"{stable_ref}^{{commit}}")
        if candidate_sha is not None:
            git("rev-parse", "--verify", f"{candidate_sha}^{{commit}}")
            if require_lineage:
                git("merge-base", "--is-ancestor", candidate_sha, release_head)
            branch_ref = candidate_sha
        base = git("rev-parse", f"{tag_ref}^{{commit}}")
        if git("merge-base", branch_ref, stable_ref) != base:
            raise ReleaseBranchesError(f"{branch}: recorded base_tag {tag} differs from its upstream merge base")
        release_range = f"{base}..{branch_ref}"
        if git("rev-list", "--count", "--merges", release_range) != "0":
            raise ReleaseBranchesError(f"{branch}: release contains merge commits above base")
        labels: list[str] = []
        for commit in git("rev-list", release_range).splitlines():
            message = git("show", "-s", "--format=%B", commit)
            declarations = [line for line in message.splitlines() if line.startswith("Branchdb-Series:")]
            if declarations:
                if len(declarations) != 1 or not re.fullmatch(r"Branchdb-Series: C[1-7]", declarations[0]):
                    raise ReleaseBranchesError(f"{branch}: invalid core-series declarations on {commit}")
                label = declarations[0].split()[-1]
                approved = entry["core_series"][label]
                if commit.lower() != approved["commit"].lower():
                    raise ReleaseBranchesError(f"{branch}: {label} differs from approved core commit")
                core_paths = git("diff-tree", "--no-commit-id", "--name-only", "-r", commit,
                                 "--", "src", "contrib/meson.build", "contrib/Makefile")
                if not core_paths:
                    raise ReleaseBranchesError(f"{branch}: {label} has no PostgreSQL core/build changes")
                source_has_core = False
                for origin_sha in approved["sources"]:
                    citations = re.findall(r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])", message.lower())
                    if not any(origin_sha.lower().startswith(citation) for citation in citations):
                        raise ReleaseBranchesError(f"{branch}: {label} does not cite approved source {origin_sha}")
                    git("rev-parse", "--verify", f"{origin_sha}^{{commit}}")
                    git("merge-base", "--is-ancestor", origin_sha, main_ref)
                    source_has_core |= bool(git("diff-tree", "--no-commit-id", "--name-only", "-r", origin_sha,
                                                "--", "src", "contrib/meson.build", "contrib/Makefile"))
                if not source_has_core:
                    raise ReleaseBranchesError(f"{branch}: {label} sources have no core/build changes")
                labels.append(label)
                continue
            paths = git("diff-tree", "--no-commit-id", "--name-only", "-r", commit,
                        "--", ".", ":!contrib/pagestore")
            subject = message.splitlines()[0] if message else ""
            if paths and not subject.startswith(("ci:", "docs:", "fixtures:")):
                raise ReleaseBranchesError(f"{branch}: unallowed release commit {commit}: {subject}")
        if sorted(labels) != [f"C{n}" for n in range(1, 8)]:
            raise ReleaseBranchesError(f"{branch}: release must contain exactly one C1-C7 commit each")
        git("rev-parse", "--verify", f"{source}^{{commit}}")
        git("merge-base", "--is-ancestor", source, main_ref)
        source_tree = git("rev-parse", f"{source}:contrib/pagestore")
        if git("cat-file", "-t", source_tree) != "tree":
            raise ReleaseBranchesError(f"{branch}: source contrib/pagestore is not a tree")
        if git("rev-parse", f"{branch_ref}:contrib/pagestore") != source_tree:
            raise ReleaseBranchesError(f"{branch}: contrib tree differs from recorded contrib_sha {source}")
        revisions[branch] = git("rev-parse", f"{branch_ref}^{{commit}}")
    return revisions


def candidate_revisions() -> dict[str, str]:
    """Exact SHA snapshots verified from the authoritative manifest."""
    return load_authoritative()["_verified_revisions"]


def supported_majors(data: dict[str, Any] | None = None) -> list[int]:
    data = data if data is not None else load()
    return sorted(entry["major"] for entry in data["supported"])


def unsupported_majors(data: dict[str, Any] | None = None) -> list[int]:
    data = data if data is not None else load()
    return sorted(entry["major"] for entry in data["unsupported"])


def candidate_branches(data: dict[str, Any] | None = None) -> list[str]:
    """Branch names whose status is "candidate", "released" or
    "released-preview" -- what a soak/acceptance workflow matrix should
    include; a "planned" major has no branch worth soaking yet."""
    data = data if data is not None else load()
    return [entry["branch"] for entry in data["supported"]
            if entry["status"] in CANDIDATE_STATUSES]


def entry_for_major(major: int, data: dict[str, Any] | None = None) -> dict[str, Any] | None:
    """The supported- or unsupported-list entry for `major`, or None."""
    data = data if data is not None else load()
    for entry in data["supported"] + data["unsupported"]:
        if entry["major"] == major:
            return entry
    return None


def _test_release_evidence(data: dict[str, Any]) -> None:
    """Exercise hand-edited promotions without changing the checked-in manifest."""
    with tempfile.TemporaryDirectory(prefix="pagestore-release-evidence-") as tmp:
        path = Path(tmp) / "release-branches.json"
        for status in sorted(CANDIDATE_STATUSES):
            promoted = copy.deepcopy(data)
            entry = promoted["supported"][0]
            major = entry["major"]
            entry.update(status=status, base_tag=f"REL_{major}_1", contrib_sha="a" * 40,
                         core_series={f"C{n}": {"commit": "b" * 40, "sources": ["a" * 40]}
                                      for n in range(1, 8)})
            path.write_text(json.dumps(promoted), encoding="utf-8")
            assert entry["branch"] in candidate_branches(load(path, verify_provenance=False))
            for field, bad_values in (
                    ("base_tag", (None, "", f"REL_{major + 1}_1", "origin/pagestore", 1)),
                    ("contrib_sha", (None, "", "abc123", "z" * 40, "origin/pagestore", 1))):
                for value in bad_values:
                    invalid = copy.deepcopy(promoted)
                    invalid["supported"][0][field] = value
                    path.write_text(json.dumps(invalid), encoding="utf-8")
                    try:
                        load(path, verify_provenance=False)
                    except ReleaseBranchesError:
                        pass
                    else:
                        raise AssertionError(f"{status} accepted invalid {field}: {value!r}")
        planned = copy.deepcopy(data)
        planned["supported"][0].update(status="planned", base_tag=None, contrib_sha=None)
        path.write_text(json.dumps(planned), encoding="utf-8")
        load(path, verify_provenance=False)


def _self_test() -> int:
    data = load()
    _test_release_evidence(data)
    majors = supported_majors(data)
    unsupported = unsupported_majors(data)
    assert len(majors) == len(set(majors)), "duplicate majors in 'supported' slipped past validation"
    assert set(majors) >= {15, 16, 17, 18, 19}, f"expected at least 15-19 supported, got {majors}"
    assert unsupported == [13, 14], f"expected exactly 13 and 14 unsupported, got {unsupported}"
    assert not (set(majors) & set(unsupported)), "a major cannot be both supported and unsupported"
    for major in unsupported:
        entry = entry_for_major(major, data)
        assert entry is not None and "EOL" in entry["reason"], \
            f"unsupported major {major} should record an EOL-shaped reason, got {entry}"
    candidates = candidate_branches(data)
    print(f"ok   - {DEFAULT_PATH} is valid: {len(majors)} supported major(s) "
          f"({', '.join(str(m) for m in majors)}), "
          f"{len(unsupported)} unsupported ({', '.join(str(m) for m in unsupported)}), "
          f"candidate/released branch(es): {', '.join(candidates) if candidates else '(none yet)'}")
    return 0


def _print_field(major: int, field: str) -> int:
    """`--major N --field FIELD`: print one field of major N's manifest
    entry (supported or unsupported), or nothing if the field is absent or
    null. Used by scripts/branchdb-sync.sh's `status` as a subprocess call,
    so the shell script does not duplicate the manifest's parsing/lookup
    logic -- this module is the one place that knows the schema."""
    try:
        entry = entry_for_major(major, load(verify_provenance=False))
    except ReleaseBranchesError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if entry is None:
        return 0
    value = entry.get(field)
    if value is not None:
        print(value)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--major", type=int, metavar="N",
                        help="print one manifest field for major N instead of self-testing")
    parser.add_argument("--field", metavar="FIELD",
                        help="the field to print (requires --major), e.g. contrib_sha, base_tag")
    parser.add_argument("--authoritative-manifest", action="store_true",
                        help="validate the manifest freshly fetched from origin/pagestore")
    parser.add_argument("--release-pr-sha", help="validate an unmerged release PR commit against authoritative metadata")
    parser.add_argument("--candidate-sha", help="validate this frozen candidate commit instead of a branch head")
    args = parser.parse_args(argv)
    if args.candidate_sha and args.release_pr_sha:
        parser.error("choose candidate-sha or release-pr-sha")
    if (args.candidate_sha or args.release_pr_sha) and not args.authoritative_manifest:
        parser.error("--candidate-sha requires --authoritative-manifest")
    if args.authoritative_manifest and (args.major is not None or args.field is not None):
        parser.error("authoritative validation cannot be combined with field lookup")
    if args.major is not None or args.field is not None:
        if args.major is None or args.field is None:
            parser.error("--major and --field must be given together")
        return _print_field(args.major, args.field)
    try:
        if args.authoritative_manifest:
            load_authoritative(candidate_sha=args.candidate_sha or args.release_pr_sha,
                               require_lineage=args.release_pr_sha is None)
            print("ok   - authoritative release manifest and Git provenance verified")
            return 0
        return _self_test()
    except (ReleaseBranchesError, AssertionError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
