#!/bin/sh
set -eu

# The post-build steps are identical on the Stamp board; run the shared
# script and then fall through to whatever build-m1.sh appended below.
# Do not exec: appended lines would never run.
"$(dirname "$0")/../stamp-p4/post-build.sh" "$@"
