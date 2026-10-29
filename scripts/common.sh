# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Shared helpers for the VCE520B build system.  Sourced by every script.

TOP=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# config.mk is a bmake fragment - `VAR ?= value` is not /bin/sh. Do not source
# it; extract the variables we need. `[?]*=` matches both `VAR = v` and `VAR ?= v`.
_mkvar() {
	sed -n "s/^$1[[:space:]]*[?]*=[[:space:]]*//p" "$TOP/config.mk" 2>/dev/null |
		sed -n '1p'
}

# config.mk is the single source of truth: a missing variable is a hard error.
: "${OPNSENSE_TARGET:=$(_mkvar OPNSENSE_TARGET)}"
: "${SRC_REPO:=$(_mkvar SRC_REPO)}"
: "${KMODS:=$(_mkvar KMODS)}"
: "${BOOT_KMODS:=$(_mkvar BOOT_KMODS)}"
: "${STOCK_KMODS:=$(_mkvar STOCK_KMODS)}"

for _v in OPNSENSE_TARGET SRC_REPO KMODS BOOT_KMODS; do
	eval "_have=\${$_v:-}"
	[ -n "$_have" ] || {
		printf 'error: %s is not defined in %s/config.mk\n' "$_v" "$TOP" >&2
		exit 1
	}
done
unset _v _have

WORK="$TOP/work"
SRC="$WORK/src"
# Each module builds in place: $TOP/src/<module> holds its Makefile and its
# sources, and MAKEOBJDIR sends the objects to $WORK/obj/<module>. There is no
# staged copy of the tree. Nothing else is built here: the stock
# mdio/etherswitch/e6000sw/ichwd .ko files come off the OPNsense image.
OUT="$TOP/out"

# What gets checked out of $SRC_REPO. `sys` must be whole - bmake generates the
# *_if.h headers from every '*_if.m' under SYSDIR. `share/mk` supplies
# bsd.kmod.mk, selected with `-m $SRC/share/mk`. Nothing else: this tree never
# builds a kernel.
SPARSE_PATHS="
sys
share/mk
"

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Resolve the profile for $OPNSENSE_TARGET into SRC_BRANCH / FREEBSD_VER /
# PATCHSET.  Uses the *_A_B variables from config.mk, where the dots in the
# release number become underscores: OPNSENSE_TARGET=26.7 -> SRC_BRANCH_26_7.
resolve_target() {
	profile=$(printf '%s' "$OPNSENSE_TARGET" | tr '.' '_')
	SRC_BRANCH=${SRC_BRANCH:-$(_mkvar "SRC_BRANCH_${profile}")}
	FREEBSD_VER=${FREEBSD_VER:-$(_mkvar "FREEBSD_${profile}")}
	PATCHSET=${PATCHSET:-$(_mkvar "PATCHSET_${profile}")}
	[ -n "$SRC_BRANCH" ] || die "unknown OPNSENSE_TARGET '$OPNSENSE_TARGET' (see config.mk)"
	PATCH_DIR="$TOP/patches/$PATCHSET"
	[ -d "$PATCH_DIR" ] || die "patch set directory $PATCH_DIR is missing"
}

# The kernel source tree we build against.  OPNsense ships its own src fork,
# so a matching OPNsense release always maps to a FreeBSD branch.
src_is_ready() {
	[ -f "$SRC/Makefile" ] && [ -d "$SRC/sys/conf" ] && [ -d "$SRC/sys/dev/e1000" ]
}
