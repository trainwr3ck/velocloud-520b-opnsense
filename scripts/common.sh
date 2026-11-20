# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Shared helpers for the VCE520B build system.  Sourced by every script.

TOP=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# config.mk is a bmake fragment: every assignment is `VAR ?= value`, which
# /bin/sh parses as a command named "OPNSENSE_TARGET" with the argument
# "?=" ("OPNSENSE_TARGET: not found").  Do not source it - extract the
# variables we need.  `[?]*=` matches both `VAR = v` and `VAR ?= v`.
_mkvar() {
	sed -n "s/^$1[[:space:]]*[?]*=[[:space:]]*//p" "$TOP/config.mk" 2>/dev/null |
		sed -n '1p'
}

# Required: config.mk is the single source of truth, so a missing variable is
# a hard error rather than a silent default.  ${VAR:=...} in an assignment
# context performs no word splitting, which matters because KMODS and
# BOOT_KMODS are space separated lists.
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
# staged copy of the tree any more - a stale stage is a bug factory. Nothing
# else is built here: the stock mdio/etherswitch/e6000sw/ichwd .ko files come
# off the OPNsense image, they are never compiled from $SRC.
OUT="$TOP/out"

# What gets checked out of $SRC_REPO.  Sparse checkout is limited to these.
#
#   sys        - the whole kernel tree. Not laziness, measured: bmake needs
#                more than the files being compiled.
#                  sys/conf, sys/tools          - kmod.mk / config.mk /
#                                                 makeobjops.awk, which generates
#                                                 bus_if.h, device_if.h, pci_if.h,
#                                                 ifdi_if.h, mdio_if.h, ...
#                  sys/dev/*, sys/net, sys/kern - every *_if.m a *_if.h is
#                                                 generated from (config.mk runs
#                                                 `find ${SYSDIR} -name '*_if.m'`)
#                  sys/sys, sys/amd64, sys/x86  - kernel headers; kmod.mk
#                                                 symlinks machine/ and x86/ into
#                                                 every module objdir
#                  sys/contrib/ck/include       - -I${SYSDIR}/contrib/ck/include
#                  sys/conf/ldscript.kmod.amd64 - -T link script for the .ko
#                A partial list under `sys` was tried and failed: the first miss
#                was `don't know how to make .../sys/dev/acpica/acpi_quirks`
#                (sys/conf/files:6-10 generates acpi_quirks.h for `optional acpi`).
#                33,279 files / 334 MB checked out; the host has 44 GB free.
#   share/mk   - bsd.kmod.mk and friends, selected with `-m $SRC/share/mk`. That
#                is also what pins SRCTOP: share/mk/src.sys.env.mk derives
#                SRCTOP from the directory bsd.init.mk was parsed out of.
#
# usr.sbin/config, lib/libkern, lib/clang and etc/mtree used to be here. They
# exist only to build a kernel with OPNsense's config(8) fork (config(8) links
# sbuf/copy from libkern; Makefile.inc1 reads lib/clang version headers; the
# kernel-tools stage runs mtree). This tree never builds a kernel - that is the
# delivery model - so they are gone. If a kernel build ever comes back, put
# them in `sys`-adjacent position again and expect the same measurement.
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
# (fetch-src.sh builds ANCHOR_opnsense_26_7 the same way.)
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
