#!/usr/bin/env python3
"""#55 rename guard: 'easystick' may only survive in

- historical/vendor paths (dated test records, docs/, hardware drop,
  README release notes), and
- '#55 compat' regions (a line containing the marker starts a region,
  '#55 compat end' closes it; a match on the marker line itself is
  also allowed).

Exits 1 and prints every offending line otherwise.
"""
import os
import re
import subprocess
import sys

ALLOWED_PATH = re.compile(
    r"^(?:vendor/|docs/|board/hardware/|README|tests/.*-20\d\d-|"
    r"linux/tests/.*-20\d\d-|linux/tests/check-no-easystick\.py$)")

BEGIN = re.compile(r"#55 compat")
END = re.compile(r"#55 compat end")


def allowed_lines(path):
    """Return the set of 1-based line numbers inside compat regions."""
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
        if path not in cache:
            cache[path] = allowed_lines(os.path.join(root, path))
        if lineno in cache[path]:
            continue
        bad.append(hit)
    for hit in bad:
        print(hit)
    if bad:
        print(f"#55 check: {len(bad)} 'easystick' reference(s) outside the "
              "allowlist (history paths or '#55 compat' regions)", file=sys.stderr)
        return 1
    print("#55 check: no 'easystick' references outside the allowlist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
