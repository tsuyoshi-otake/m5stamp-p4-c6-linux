#!/usr/bin/env bash
# Initialise the locked upstream submodules or fetch equivalent trees into an
# external cache.  Generated toolchains and build outputs stay external.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
firmware_root=$(cd -- "${script_dir}/.." && pwd)
repo_root=$(cd -- "${firmware_root}/../../.." && pwd)

if [[ $# -eq 1 && "$1" == "--vendor" ]]; then
  command -v git >/dev/null || { echo "git is required" >&2; exit 1; }
  git -C "${repo_root}" submodule sync --recursive
  git -C "${repo_root}" submodule update --init --recursive
  python3 "${firmware_root}/tools/verify-source-lock.py"
  exit 0
fi

if [[ $# -ne 1 ]]; then
  echo "usage: $0 --vendor | /absolute/path/to/external-source-cache" >&2
  exit 2
fi

lock_file=${firmware_root}/versions.lock.json
destination=$(python3 - "$1" <<'PY'
from pathlib import Path
import sys
print(Path(sys.argv[1]).expanduser().resolve())
PY
)

case "${destination}" in
  "${repo_root}"|"${repo_root}"/*)
    echo "refusing to clone sources inside the repository: ${destination}" >&2
    exit 1
    ;;
esac

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }
mkdir -p "${destination}"

python3 - "${lock_file}" "${destination}" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

lock_path = Path(sys.argv[1])
destination = Path(sys.argv[2])
sources = json.loads(lock_path.read_text(encoding="utf-8"))["sources"]

def run(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()

for name, source in sources.items():
    path = destination / name
    repository = source.get("fetch_repository", source["repository"])
    commit = source["commit"]
    tag = source.get("tag")
    if path.exists() and not (path / ".git").exists():
        raise SystemExit(f"refusing non-git destination: {path}")
    if not path.exists():
        print(f"initialise shallow checkout {name}")
        path.mkdir(parents=True)
        subprocess.check_call(("git", "init", str(path)))
        subprocess.check_call(("git", "remote", "add", "origin", repository), cwd=path)
    else:
        current_remote = run("git", "config", "--get", "remote.origin.url", cwd=path)
        if current_remote.rstrip("/") != repository.rstrip("/"):
            raise SystemExit(f"origin mismatch for {name}: {current_remote}")
    # A full Linux history is multiple gigabytes. Fetch only the locked ref,
    # with no blobs until the build actually checks out a file. Tagged releases
    # use the tag ref; the WHY2025 reference is pinned by its immutable commit.
    ref = f"refs/tags/{tag}:refs/tags/{tag}" if tag else commit
    subprocess.check_call(("git", "fetch", "--depth=1", "--filter=blob:none", "origin", ref), cwd=path)
    subprocess.check_call(("git", "checkout", "--detach", commit), cwd=path)
    actual = run("git", "rev-parse", "HEAD", cwd=path)
    if actual != commit:
        raise SystemExit(f"checkout mismatch for {name}: {actual} != {commit}")
    print(f"locked {name} at {actual}")
PY
