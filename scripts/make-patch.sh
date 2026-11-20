#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# ---------------------------------------------------------------------------
# make-patch.sh - regenerate a patch set from a modified work/src tree.
#
# When upstream moves (a new OPNsense release, a rebased stable/NN.x branch)
# and an existing series no longer applies, do this:
#
#   1. scripts/fetch-src.sh                  # get the new tree
#   2. git -C work/src apply -p1 --reject patches/<old>/velocloud-vce520b.patch
#      git -C work/src status                # shows *.rej files
#   3. fix every .rej by hand in work/src (the semantics are documented in
#      README.md, section "What the patch series actually changes")
#   4. scripts/make-patch.sh <new-patchset-name>
#
# The tool diffs work/src against its own pristine HEAD, so the result is
# always relative to the exact commit fetch-src.sh checked out.  It only ever
# emits hunks under the paths the port is allowed to touch.
#
#   usage: scripts/make-patch.sh <patchset> [--dry-run]
# ---------------------------------------------------------------------------
set -eu

. "$(dirname -- "$0")/common.sh"

PATCHSET=${1:-}
[ -n "$PATCHSET" ] || die "usage: scripts/make-patch.sh <patchset-name> [--dry-run]"
DRY_RUN=0
[ "${2:-}" = "--dry-run" ] && DRY_RUN=1

src_is_ready || die "no src tree - run scripts/fetch-src.sh first"

# Paths the VeloCloud port is allowed to modify.  Anything else in the tree
# must stay pristine, and is filtered out so an accidental edit cannot leak
# into a release patch set.
#
# sys/dev/etherswitch is deliberately NOT here. The committed patch set ships a
# stock e6000sw.ko, so keeping the path out of PATHS means a stale edit in a
# working tree cannot leak back into a release. The STU/dot1q work on the
# 88E6176 is real and is recorded in config.mk, but it is not in this patch
# set; landing it means adding this path back deliberately, along with e6000sw
# in config.mk KMODS and BOOT_KMODS - see README.md, "VLANs".
PATHS="sys/dev/e1000"

log "diffing $SRC against HEAD ($(git -C "$SRC" rev-parse --short HEAD))"

patch_file="$TOP/patches/$PATCHSET/velocloud-vce520b.patch"
tmp="$WORK/make-patch.tmp"
mkdir -p "$WORK"

# git diff inside the tree gives correct a/ b/ prefixes and LF line endings,
# which is exactly what `git apply -p1` wants.  (Diffing two loose directory
# copies with `git diff --no-index` instead produces Windows-style quoted
# paths and, if written out with PowerShell, CRLF endings - both break
# `git apply`.  Avoid that path entirely.)
git -C "$SRC" diff --no-color -- $PATHS > "$tmp"

if [ ! -s "$tmp" ]; then
	die "no changes under $PATHS - nothing to record"
fi

# Normalise: strip any trailing CR that a Windows checkout may have introduced.
tr -d '\r' < "$tmp" > "$WORK/make-patch.lf"
mv "$WORK/make-patch.lf" "$tmp"

files=$(grep -c '^diff --git ' "$tmp")
adds=$(grep -c '^+[^+]' "$tmp" || true)
log "$files file(s) changed, ~$adds added lines"
grep '^diff --git ' "$tmp" | sed 's/^diff --git a\///; s/ b\/.*$//' | sed 's/^/    /'

if [ "$DRY_RUN" = 1 ]; then
	log "--dry-run: not writing $patch_file"
	exit 0
fi

mkdir -p "$TOP/patches/$PATCHSET"
if [ -f "$patch_file" ]; then
	cp "$patch_file" "$patch_file.bak"
	log "previous series kept as $(basename "$patch_file").bak"
fi
cp "$tmp" "$patch_file"
log "wrote patches/$PATCHSET/velocloud-vce520b.patch"

# Verify round-trip: revert, then re-apply.  A series that does not survive
# this is not releasable.
log "verifying the series applies to a pristine tree"
git -C "$SRC" apply --reverse "$patch_file"
if git -C "$SRC" apply -p1 --check "$patch_file"; then
	git -C "$SRC" apply -p1 "$patch_file"
	log "verified: patches/$PATCHSET applies cleanly"
else
	die "the series we just wrote does not re-apply - investigate before using it"
fi

cat <<EOF

Remember to record the new anchor in config.mk:
  ANCHOR_<patchset, dots and dashes as underscores> ?= $(git -C "$SRC" rev-parse HEAD)
and, if this patch set belongs to a new release, add the matching
SRC_BRANCH_/FREEBSD_/PATCHSET_ profile for it.
EOF
