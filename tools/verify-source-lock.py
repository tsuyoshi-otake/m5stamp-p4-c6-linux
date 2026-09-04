#!/usr/bin/env python3
"""Validate that the firmware source manifest is immutable and complete."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


LOCK = Path(__file__).resolve().parents[1] / "versions.lock.json"
REPO_ROOT = LOCK.parents[3]
SHA1 = re.compile(r"^[0-9a-f]{40}$")
REQUIRED = {
    "why2025_linux_reference",
    "linux_base",
    "buildroot",
    "esp_idf",
    "esp_hosted_ng",
}


def main() -> int:
    try:
        data = json.loads(LOCK.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"FAIL: cannot read {LOCK}: {exc}")
        return 1

    sources = data.get("sources")
    if not isinstance(sources, dict):
        print("FAIL: sources must be an object")
        return 1

    failures: list[str] = []
    missing = REQUIRED - sources.keys()
    failures.extend(f"missing source: {name}" for name in sorted(missing))
    for name, source in sources.items():
        if not isinstance(source, dict):
            failures.append(f"{name}: source must be an object")
            continue
        repository = source.get("repository", "")
        commit = source.get("commit", "")
        if not isinstance(repository, str) or not repository.startswith("https://"):
            failures.append(f"{name}: repository must be an HTTPS URL")
        if not isinstance(commit, str) or not SHA1.fullmatch(commit):
            failures.append(f"{name}: commit must be a 40-character lowercase SHA-1")
        if not source.get("role"):
            failures.append(f"{name}: role is required")

    # The locked sources are intentionally represented by gitlinks in the
    # parent repository.  Check both the URL/path manifest and the index mode;
    # a merely present directory is not sufficient evidence of pinning.
    gitmodules = REPO_ROOT / ".gitmodules"
    if not gitmodules.is_file():
        failures.append(".gitmodules is missing while source submodules are required")
    else:
        try:
            path_lines = subprocess.check_output(
                ("git", "config", "--file", str(gitmodules), "--get-regexp",
                 r"^submodule\..*\.path$"),
                cwd=REPO_ROOT, text=True, stderr=subprocess.STDOUT,
            ).splitlines()
        except (OSError, subprocess.CalledProcessError) as exc:
            failures.append(f"cannot read .gitmodules: {exc}")
            path_lines = []

        configured: dict[str, tuple[str, str]] = {}
        for line in path_lines:
            key, path = line.split(None, 1)
            section = key.removesuffix(".path")
            try:
                url = subprocess.check_output(
                    ("git", "config", "--file", str(gitmodules), "--get",
                     f"{section}.url"),
                    cwd=REPO_ROOT, text=True, stderr=subprocess.STDOUT,
                ).strip()
            except (OSError, subprocess.CalledProcessError) as exc:
                failures.append(f"{section}: cannot read submodule URL: {exc}")
                continue
            configured[path] = (url, section)

        for name, source in sources.items():
            path = source.get("submodule_path")
            if not isinstance(path, str) or not path:
                failures.append(f"{name}: submodule_path is required")
                continue
            expected_url = source.get("submodule_repository", source.get("repository", ""))
            actual = configured.get(path)
            if actual is None:
                failures.append(f"{name}: submodule path is not configured: {path}")
                continue
            if actual[0].rstrip("/") != expected_url.rstrip("/"):
                failures.append(f"{name}: submodule URL mismatch: {actual[0]} != {expected_url}")
            try:
                stage_lines = subprocess.check_output(
                    ("git", "ls-files", "--stage", "--", path),
                    cwd=REPO_ROOT, text=True, stderr=subprocess.STDOUT,
                ).splitlines()
            except (OSError, subprocess.CalledProcessError) as exc:
                failures.append(f"{name}: cannot inspect gitlink: {exc}")
                continue
            if len(stage_lines) != 1:
                failures.append(f"{name}: expected exactly one staged gitlink at {path}")
                continue
            mode, staged_commit, _stage, staged_path = stage_lines[0].split(None, 3)
            if mode != "160000" or staged_path != path:
                failures.append(f"{name}: {path} is not a gitlink (mode {mode})")
            if staged_commit != source.get("commit"):
                failures.append(f"{name}: gitlink {staged_commit} != locked {source.get('commit')}")

    if failures:
        print("\n".join(f"FAIL: {failure}" for failure in failures))
        return 1
    print(f"PASS: {len(sources)} immutable sources in {LOCK.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
