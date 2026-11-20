#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Inject the VeloCloud modification into the fetched kernel source tree.
#
#   1. apply patches/<patchset>/velocloud-vce520b.patch to sys/dev/e1000
#
# Nothing is copied into $SRC/sys/dev: this tree never builds a kernel, and
# scripts/build.sh reaches our sources through .PATH / VELOVC. The src tree is
# a git checkout, so `scripts/prepare.sh --revert` (or
# `git -C work/src checkout -- .`) puts it back to pristine.

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
	# --allow-extern: the series was produced with git diff, which is fine
	# for a plain -p1 apply inside the src tree.
	if ! git -C "$SRC" apply -p1 --check \
		"$PATCH_DIR/velocloud-vce520b.patch" 2>"$WORK/apply.log"; then
		sed -n '1,40p' "$WORK/apply.log" >&2
		die "patch set $(basename "$PATCH_DIR") does not apply to $SRC_BRANCH.
Try 'git -C $SRC apply -p1 --reject $PATCH_DIR/velocloud-vce520b.patch',
fix the rejected hunks against the current tree, and regenerate the patch
with scripts/make-patch.sh."
	fi
	git -C "$SRC" apply -p1 "$PATCH_DIR/velocloud-vce520b.patch"
fi

# Sanity checks: the patch touches exactly two files, both in sys/dev/e1000.
# mdio, etherswitch and e6000sw are unpatched in this tree - stock .ko files
# from /boot/kernel are used as shipped (config.mk, README "VLANs").
grep -q VeloCloud "$SRC/sys/dev/e1000/if_em.c" ||
	die "sys/dev/e1000/if_em.c has no VeloCloud modification"
grep -q VeloCloud "$SRC/sys/dev/e1000/e1000_82575.c" ||
	die "sys/dev/e1000/e1000_82575.c has no VeloCloud modification"
grep -q igb_vc_probe "$SRC/sys/dev/e1000/e1000_82575.c" ||
	die "igb_vc_probe hook missing from e1000_82575.c"
# e6000sw is stock here. If a working tree carries the STU/dot1q work, that is
# a deliberate change and config.mk has to say so - warn, do not die, and do
# not let it ride along silently in a build of four modules.
if grep -q VeloCloud "$SRC/sys/dev/etherswitch/e6000sw/e6000sw.c"; then
	warn "e6000sw.c is modified but this tree ships it stock - regenerate the
patch set with sys/dev/etherswitch in PATHS, or restore the file from git"
fi

log "src prepared for OPNsense $OPNSENSE_TARGET (FreeBSD $FREEBSD_VER)"
