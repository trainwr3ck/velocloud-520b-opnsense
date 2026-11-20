#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Fetch the kernel source tree that matches $OPNSENSE_TARGET.
#
# Sparse checkout keeps the clone to sys/ plus the handful of userland
# directories the build needs, instead of the full ~2 GB src tree.  sys/ is
# checked out whole because a kernel build needs all of it - see SPARSE_PATHS
# in common.sh for the measurement.

set -eu
. "$(dirname "$0")/common.sh"

resolve_target

if src_is_ready; then
	current=$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
	if [ "$current" = "$SRC_BRANCH" ]; then
		log "src tree already present at $SRC ($SRC_BRANCH)"
		# SPARSE_PATHS may have widened since this tree was configured.  Skipping
		# straight past the sparse step is how a kernel build ends up with
		#   don't know how to make .../work/src/sys/dev/acpica/acpi_quirks
		# Re-applying is idempotent: git only materialises paths that are
		# missing, and it does not touch files already checked out (patched or
		# not), because the new cone is a superset of the old one.
		log "re-applying sparse checkout"
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

anchor_var="ANCHOR_$(printf '%s' "opnsense_$OPNSENSE_TARGET" | tr '.' '_')"
# config.mk is not sourced (its `?=` assignments are not /bin/sh), so the
# anchor has to be read out of it like every other variable. Reading it with a
# bare eval was dead code: the variable is never in the environment, so the
# comparison below always saw an empty anchor and stayed silent. Measured -
# a fresh clone landed on stable/26.7 tip c962f76493b6, not the verified
# 12334a596709, and printed no warning at all.
anchor=$(_mkvar "$anchor_var")
head=$(git -C "$SRC" rev-parse HEAD)
log "src ready: $SRC_BRANCH @ $head"
if [ -n "$anchor" ] && [ "$head" != "$anchor" ]; then
	warn "HEAD $head differs from the verified anchor $anchor."
	warn "The patch set may need offsets (git apply will tell you), and the .ko"
	warn "will NOT match a stock image whose kernel came from the anchor commit."
	warn "Pin it: git -C $SRC fetch --depth 1 origin $anchor && git checkout FETCH_HEAD"
fi
