"""Explicit provenance for fixture captures; never silently reuse old SHAs."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


class CaptureSourceError(ValueError):
    pass


def validate_capture_source(value: Any) -> dict[str, Any]:
    keys = {"pagestore_sha", "postgres_core_sha", "postgres_major", "cassert"}
    if not isinstance(value, dict) or set(value) != keys:
        raise CaptureSourceError("capture_source needs exactly " + ", ".join(sorted(keys)))
    for key in ("pagestore_sha", "postgres_core_sha"):
        if not isinstance(value[key], str) or not re.fullmatch(r"[0-9a-fA-F]{40}", value[key]):
            raise CaptureSourceError(f"capture_source.{key} must be a full Git SHA")
    if type(value["postgres_major"]) is not int or value["postgres_major"] <= 0:
        raise CaptureSourceError("capture_source.postgres_major must be a positive integer")
    if type(value["cassert"]) is not bool:
        raise CaptureSourceError("capture_source.cassert must be a JSON boolean")
    return dict(value)


def add_capture_source_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--capture-source", type=Path, metavar="JSON",
                        help="--capture only: provenance JSON containing pagestore_sha, "
                             "postgres_core_sha, postgres_major and cassert; required "
                             "when replacing a capture that already records provenance")


def capture_source(args: argparse.Namespace, fixture: Path) -> dict[str, Any] | None:
    source = getattr(args, "capture_source", None)
    try:
        if source is not None:
            return validate_capture_source(json.loads(source.read_text(encoding="utf-8")))
        previous = fixture / "fixture.json"
        if previous.is_file() and "capture_source" in json.loads(previous.read_text(encoding="utf-8")):
            raise CaptureSourceError("recapture needs --capture-source; refusing to drop or "
                                     "reuse the previous capture's provenance")
    except (OSError, json.JSONDecodeError) as error:
        raise CaptureSourceError(f"cannot read capture provenance: {error}") from error
    return None


def require_release_capture(metadata: dict[str, Any]) -> None:
    """A matching current fixture needs capture evidence for release gates."""
    if "capture_source" not in metadata:
        raise CaptureSourceError("a matching current fixture needs capture_source for "
                                 "--require-build-match; recapture with --capture-source")
    validate_capture_source(metadata["capture_source"])
