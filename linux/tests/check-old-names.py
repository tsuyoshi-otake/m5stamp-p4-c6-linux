#!/usr/bin/env python3
"""#55b rename guard: 'easystick' may only survive in

- vendor/ (third-party submodules),
- board/hardware/ (the carrier's design files),
- the #55 rename record linux/tests/issue55-rename-2026-10-23.md
  (it lists the old names by necessity),
- '#55 compat' regions (one-release aliases; '#55 compat' starts a
  region, '#55 compat end' closes it), and
- '55b keep' regions (the README rename note and the v0.4.2
  checksum/asset lines that quote real published file names;
  '55b keep' starts a region, '55b keep end' closes it).

A match on a marker line itself is allowed either way.  Lines naming
this checker (its own invocation in build-m1.sh) are skipped.

Scans the whole tree (git grep when a .git dir is present, walk
otherwise).  Exits 1 and prints every offending line otherwise.
"""
import os
import re
import subprocess
import sys

ALLOWED_PATH = re.compile(
    r"^(?:vendor/|board/hardware/|"
    r"linux/tests/issue55-rename-2026-10-23\.md$|"
    r"linux/tests/check-old-names\.py$)")

BEGIN = re.compile(r"#55 compat|55b keep")
END = re.compile(r"#55 compat end|55b keep end")


def allowed_lines(path):
    """Return the set of 1-based line numbers inside keep regions."""
    ok = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            inside = False
            for i, line in enumerate(f, 1):
                if END.search(line):
                    ok.add(i)
                    inside = False
                    continue
                if BEGIN.search(line):
                    inside = True
                if inside:
                    ok.add(i)
    except OSError:
        pass
    return ok


NEEDLE = re.compile(r"easystick", re.IGNORECASE)


def collect_hits(root):
    if os.path.isdir(os.path.join(root, ".git")):
        out = subprocess.run(
            ["git", "grep", "-in", "easystick"], cwd=root,
            capture_output=True, text=True, encoding="utf-8",
            errors="replace").stdout
        for line in out.splitlines():
            m = re.match(r"([^:]+):(\d+):", line)
            if m:
                yield m.group(1), int(m.group(2)), line
        return
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            try:
                with open(full, encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        if NEEDLE.search(line):
                            yield rel, i, f"{rel}:{i}:{line.rstrip()}"
            except OSError:
                continue


def main():
    if len(sys.argv) > 1:
        root = os.path.abspath(sys.argv[1])
    else:
        root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True).strip()
    bad = []
    cache = {}
    for path, lineno, hit in collect_hits(root):
        if ALLOWED_PATH.search(path):
            continue
        if "check-old-names" in hit:
            continue
        if path not in cache:
            cache[path] = allowed_lines(os.path.join(root, path))
        if lineno in cache[path]:
            continue
        bad.append(hit)
    for hit in bad:
        print(hit)
    if bad:
        print(f"#55b check: {len(bad)} 'easystick' reference(s) outside the "
              "allowlist (exceptions or keep/compat regions)", file=sys.stderr)
        return 1
    print("#55b check: no 'easystick' references outside the allowlist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
