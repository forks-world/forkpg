#!/usr/bin/env python3
"""Capture and replay an immutable pagestore/PostgreSQL WAL workload.

All paths are explicit and new: existing work directories and fixtures are
never overwritten or removed. See ENDURANCE.md for the workflow and limits.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import threading
import time

from pagestore_endurance import Failure, Run, WORKLOAD, quoted


GIB = 1024 ** 3


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_hash(root):
    """Hash the exact regular-file set; never follow links out of a fixture."""
    if root.is_symlink() or not root.is_dir():
        raise Failure("fixture_type", f"fixture root is not a directory: {root}")
    result = {".": {"type": "directory", "mode": stat.S_IMODE(root.stat().st_mode)}}
    def walk_error(error):
        raise error
    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        dirs.sort()
        files.sort()
        for name in dirs + files:
            path = Path(directory) / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise Failure("fixture_type", f"unsupported fixture entry: {path}")
            if stat.S_ISDIR(mode):
                result[str(path.relative_to(root)) + "/"] = {
                    "type": "directory", "mode": stat.S_IMODE(mode)}
        for name in files:
            path = Path(directory) / name
            result[str(path.relative_to(root))] = {
                "type": "file", "mode": stat.S_IMODE(path.stat().st_mode), "sha256": file_hash(path)}
    return result


def assert_separate(a, b):
    if a == b or a in b.parents or b in a.parents:
        raise Failure("paths_overlap", f"work and fixture paths overlap: {a}, {b}")


def check_free(path, min_free_gb, required_bytes=0):
    if shutil.disk_usage(path).free - required_bytes < min_free_gb * GIB:
        raise Failure("disk_free", f"free space below {min_free_gb} GiB at {path}")


def allocated_bytes(root):
    return sum(path.stat().st_blocks * 512 for path in root.rglob("*") if path.is_file())


def copy_tree(src, dst, args, copied):
    """Copy without links, checking filesystem headroom during the copy."""
    def copy(src_file, dst_file):
        if Path(src_file).is_symlink():
            raise Failure("fixture_type", f"symlink in input: {src_file}")
        size = os.stat(src_file).st_size
        copied[0] += size
        if copied[0] > args.max_run_gb * GIB:
            raise Failure("run_size", "copied input exceeds the run-size budget")
        check_free(Path(dst_file).parent, args.min_free_gb, size)
        return shutil.copy2(src_file, dst_file, follow_symlinks=False)
    shutil.copytree(src, dst, symlinks=True, copy_function=copy)


def workload(run, per_client):
    argv = [str(run.bin / "pgbench"), "-n", "-h", "127.0.0.1", "-p", str(run.writer.port),
            "-U", "postgres", "-c", str(run.args.clients), "-j", str(min(run.args.clients, 4)),
            "-t", str(per_client), "--random-seed=246813579", "-D", f"naccts={run.naccts}",
            "-D", f"ndocs={run.ndocs}"]
    for name, (weight, _) in WORKLOAD.items():
        argv += ["-f", f"{run.root / 'workload' / (name + '.sql')}@{weight}"]
    with (run.root / "pgbench.log").open("w") as log:
        process = subprocess.Popen(argv + ["postgres"], env=run.env, stdout=log,
                                   stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 1800
        try:
            while process.poll() is None:
                run.check_health()
                if time.monotonic() >= deadline:
                    raise Failure("workload_timeout", "pgbench exceeded 1800 seconds")
                time.sleep(0.2)
            run.check_health()
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if process.returncode != 0:
        output = (run.root / "pgbench.log").read_text(errors="replace")
        raise Failure("workload_failed", f"pgbench rc={process.returncode}: {output[-2000:]}")


def installed_module(run):
    result = subprocess.run([str(run.bin / "pg_config"), "--pkglibdir"], env=run.env,
                            check=True, capture_output=True, text=True, timeout=30)
    directory = Path(result.stdout.strip())
    module = directory / "pagestore.so"
    if not directory.is_absolute() or not module.is_file():
        raise Failure("build_incomplete", f"pagestore module is absent from {directory}")
    return module


def start_sampling(run):
    run.sampler_thread = threading.Thread(target=run.sampler, daemon=True)
    run.sampler_thread.start()


def capture(run, args):
    fixture = args.fixture
    fixture.mkdir(parents=True, exist_ok=False)
    check_free(fixture, args.min_free_gb)
    start_sampling(run)
    run.provision()
    run.check_health()
    run.verify_materializer("fixed replay baseline", amcheck=False)
    run.mat.stop("fast")
    workload(run, args.transactions_per_client)
    run.resolve_prepared(run.writer)
    target = run.archive_current_wal()
    checksums = run.checksums(run.writer)
    run.check_invariants(run.writer, False)
    run.check_health()
    run.writer.stop("fast")
    run.stop_sampling()
    run.check_health()
    run.stop_daemon()
    if allocated_bytes(run.store) > args.max_store_gb * GIB:
        raise Failure("store_size", "captured store exceeds the store-size budget")
    copied = [0]
    source_files = {}
    for name in ("store", "materializer"):
        # The source is quiescent and must not refer outside the snapshot.
        source_files[name] = tree_hash(run.root / name)
        copy_tree(run.root / name, fixture / name, args, copied)
    files = {name: tree_hash(fixture / name) for name in ("store", "materializer")}
    if files != source_files:
        raise Failure("fixture_integrity", "captured copy differs from its source")
    # Layer manifests contain absolute paths. A copied store is recognized as a
    # relocated store only when the original parent no longer exists. Preserve
    # the source under a different name instead of deleting it.
    run.store.rename(run.root / "captured-store")
    metadata = {"version": 2, "target_lsn": target, "checksums": checksums,
                "scale": args.scale, "clients": args.clients,
                "transactions_per_client": args.transactions_per_client, "files": files,
                "capture_daemon_sha256": file_hash(run.daemon_bin),
                "capture_postgres_sha256": file_hash(run.bin / "postgres")}
    temporary = fixture / "fixture.json.tmp"
    temporary.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, fixture / "fixture.json")
    run.event("fixture_captured", target=target, files=sum(map(len, files.values())), fixture=str(fixture))
    return {"fixture": str(fixture), "target_lsn": target,
            "fixture_manifest_sha256": file_hash(fixture / "fixture.json")}


def validate_fixture(fixture):
    meta_path = fixture / "fixture.json"
    meta = json.loads(meta_path.read_text())
    if meta.get("version") != 2 or set(meta.get("files", {})) != {"store", "materializer"}:
        raise Failure("fixture_format", "unsupported or incomplete fixture manifest")
    if not isinstance(meta.get("target_lsn"), str) or not re.fullmatch(
            r"[0-9A-Fa-f]{1,8}/[0-9A-Fa-f]{1,8}", meta["target_lsn"]):
        raise Failure("fixture_format", "invalid target LSN")
    if not isinstance(meta.get("checksums"), dict):
        raise Failure("fixture_format", "invalid checksums")
    for name in ("store", "materializer"):
        if tree_hash(fixture / name) != meta["files"][name]:
            raise Failure("fixture_integrity", f"fixture {name} does not match its manifest")
    return meta


def replay(run, args):
    fixture = args.fixture
    meta_path = fixture / "fixture.json"
    meta = validate_fixture(fixture)
    copied = [0]
    for name in ("store", "materializer"):
        copy_tree(fixture / name, run.root / name, args, copied)
        if tree_hash(run.root / name) != meta["files"][name]:
            raise Failure("fixture_integrity", f"copied {name} does not match the fixture")
    if allocated_bytes(run.store) > args.max_store_gb * GIB:
        raise Failure("store_size", "copied store exceeds the store-size budget")
    run.naccts = d_naccts(meta["scale"])
    segsize = run.wal_segment_size(run.mat.datadir)
    with (run.mat.datadir / "postgresql.conf").open("a") as f:
        f.write(f"\npagestore.localsvc_shm = {quoted(run.shm)}\nport = {run.mat.port}\n"
                f"restore_command = {quoted(run.restore_command(0, segsize))}\n")
    start_sampling(run)
    run.start_daemon()
    started = time.monotonic()
    run.mat.start()
    target = meta["target_lsn"]
    run.wait_for("fixed replay target", lambda: run.mat.sql(
        f"SELECT pg_last_wal_replay_lsn() >= '{target}'::pg_lsn;", timeout=run.probe_timeout()) == "t",
        args.sync_timeout)
    elapsed = time.monotonic() - started
    got = run.checksums(run.mat)
    if got != meta["checksums"]:
        diff = {k: (meta["checksums"].get(k), got.get(k)) for k in set(meta["checksums"]) | set(got)
                if meta["checksums"].get(k) != got.get(k)}
        raise Failure("content_mismatch", str(diff))
    run.check_invariants(run.mat, False)
    run.check_health()
    run.event("fixed_replay_verified", target=target, replay_s=round(elapsed, 3), tables=len(got))
    run.mat.stop("fast")
    run.stop_sampling()
    run.check_health()
    run.stop_daemon()
    module = installed_module(run)
    return {"target_lsn": target, "replay_seconds": round(elapsed, 3), "checksums": got,
            "daemon_args": args.daemon_arg, "sample_interval": args.sample_interval,
            "fixture_manifest_sha256": file_hash(meta_path),
            "daemon_sha256": file_hash(run.daemon_bin), "postgres_sha256": file_hash(run.bin / "postgres"),
            "module_sha256": file_hash(module), "walrestore_sha256": file_hash(run.walrestore)}


def d_naccts(scale):
    from pagestore_endurance import NACCTS_PER_SCALE
    if not isinstance(scale, int) or scale <= 0:
        raise Failure("fixture_format", "invalid fixture scale")
    return scale * NACCTS_PER_SCALE


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=("capture", "replay"))
    p.add_argument("--build", required=True)
    p.add_argument("--work-dir", type=Path, required=True, help="new, preserved run directory")
    p.add_argument("--fixture", type=Path, required=True, help="new capture or existing immutable fixture")
    p.add_argument("--seed", type=int, default=2)
    p.add_argument("--scale", type=int, default=1)
    p.add_argument("--clients", type=int, default=2)
    p.add_argument("--transactions-per-client", type=int, default=15000)
    p.add_argument("--sync-timeout", type=float, default=600)
    p.add_argument("--sample-interval", type=float, default=5)
    p.add_argument("--max-run-gb", type=float, default=20)
    p.add_argument("--max-store-gb", type=float, default=15)
    p.add_argument("--min-free-gb", type=float, default=50)
    p.add_argument("--max-rss-mb", type=int, default=8192)
    p.add_argument("--max-fds", type=int, default=8192)
    p.add_argument("--daemon-arg", action="append", default=[])
    args = p.parse_args()
    for name in ("scale", "clients", "transactions_per_client", "sync_timeout", "sample_interval",
                 "max_run_gb", "max_store_gb", "min_free_gb", "max_rss_mb", "max_fds"):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            p.error(f"--{name.replace('_', '-')} must be positive and finite")
    args.work_dir = args.work_dir.resolve()
    args.fixture = args.fixture.resolve()
    try:
        assert_separate(args.work_dir, args.fixture)
        args.work_dir.mkdir(parents=True, exist_ok=False)
        check_free(args.work_dir, args.min_free_gb)
        run = Run(args, args.seed, args.work_dir)
    except (Failure, OSError) as error:
        print(f"setup failed: {error}", file=sys.stderr)
        return 1
    ok = False
    result = None
    try:
        result = capture(run, args) if args.mode == "capture" else replay(run, args)
        ok = True
    except (Failure, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"{args.mode} failed: {error}", file=sys.stderr)
        (args.work_dir / "failure.txt").write_text(str(error) + "\n")
    finally:
        run.teardown(preserve_state=not ok)
    if ok:
        (args.work_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
