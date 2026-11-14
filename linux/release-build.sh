#!/bin/sh
# Release build flags in exactly one place (tab5 #8): the user-pool
# allocator options the release image is built with.  release.yml and the
# manual verification build both call this wrapper so the two cannot drift.
# Everything after the flags is the normal build-c68.sh invocation.
exec env \
	NOMMU_POOL_BESTFIT=1 \
	NOMMU_USER_RESERVE=1 \
	NOMMU_SYSTEM_MARK=1 \
	bash "$(dirname "$0")/build-c68.sh" "$@"
