#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Inject the VeloCloud modification into the fetched kernel source tree:
# apply patches/<patchset>/velocloud-vce520b.patch to sys/dev/e1000.
#
# `scripts/prepare.sh --revert` puts the src tree back to pristine.

set -eu
. "$(dirname "$0")/common.sh"

resolve_target

src_is_ready || die "no src tree - run scripts/fetch-src.sh first"

if [ "${1:-}" = "--revert" ]; then
	log "reverting $SRC"
	git -C "$SRC" checkout -- .
	rm -rf "$WORK/obj"
	exit 0
fi

# --------------------------------------------------------------------------
# 1. upstream patches
# --------------------------------------------------------------------------
if git -C "$SRC" apply --reverse --check "$PATCH_DIR/velocloud-vce520b.patch" 2>/dev/null; then
	log "patch set $(basename "$PATCH_DIR") already applied"
else
	log "applying $(basename "$PATCH_DIR")/velocloud-vce520b.patch"
	if ! git -C "$SRC" apply -p1 --check \
		"$PATCH_DIR/velocloud-vce520b.patch" 2>"$WORK/apply.log"; then
		sed -n '1,40p' "$WORK/apply.log" >&2
		die "patch set $(basename "$PATCH_DIR") does not apply to $SRC_BRANCH.
grep -A1 'anchored to' $TOP/src/vce520b/igb_vc.c"
	fi
	git -C "$SRC" apply -p1 "$PATCH_DIR/velocloud-vce520b.patch"
fi

# Sanity checks.
grep -q VeloCloud "$SRC/sys/dev/e1000/if_em.c" ||
	die "sys/dev/e1000/if_em.c has no VeloCloud modification"
grep -q VeloCloud "$SRC/sys/dev/e1000/e1000_82575.c" ||
	die "sys/dev/e1000/e1000_82575.c has no VeloCloud modification"
grep -q igb_vc_probe "$SRC/sys/dev/e1000/e1000_82575.c" ||
	die "igb_vc_probe hook missing from e1000_82575.c"
if grep -q VeloCloud "$SRC/sys/dev/etherswitch/e6000sw/e6000sw.c"; then
	die "e6000sw.c is modified - that patch was dropped, restore it from git"
fi

log "src prepared for OPNsense $OPNSENSE_TARGET (FreeBSD $FREEBSD_VER)"
