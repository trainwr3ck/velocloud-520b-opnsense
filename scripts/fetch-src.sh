#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Fetch the kernel source tree that matches $OPNSENSE_TARGET.
# Sparse checkout: sys/ plus share/mk only - this tree never builds a kernel.

set -eu
. "$(dirname "$0")/common.sh"

resolve_target

if src_is_ready; then
	current=$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
	if [ "$current" = "$SRC_BRANCH" ]; then
		log "src tree already present at $SRC ($SRC_BRANCH)"
		git -C "$SRC" sparse-checkout init --cone
		# shellcheck disable=SC2086
		git -C "$SRC" sparse-checkout set $SPARSE_PATHS
		exit 0
	fi
	log "src tree is on $current, switching to $SRC_BRANCH"
fi

mkdir -p "$WORK"

if [ ! -d "$SRC/.git" ]; then
	log "cloning $SRC_REPO ($SRC_BRANCH, sparse)"
	git clone --depth 1 --branch "$SRC_BRANCH" --filter=blob:none \
		--no-checkout "$SRC_REPO" "$SRC"
else
	git -C "$SRC" remote set-branches origin "$SRC_BRANCH"
	git -C "$SRC" fetch --depth 1 origin "$SRC_BRANCH"
	git -C "$SRC" checkout -B "$SRC_BRANCH" "origin/$SRC_BRANCH"
fi

log "enabling sparse checkout"
git -C "$SRC" sparse-checkout init --cone
# shellcheck disable=SC2086
git -C "$SRC" sparse-checkout set $SPARSE_PATHS
git -C "$SRC" checkout

log "src ready: $SRC_BRANCH @ $(git -C "$SRC" rev-parse HEAD)"
