#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# ---------------------------------------------------------------------------
# build.sh - compile the VeloCloud VCE520B kernel modules against the
#            OPNsense source tree prepared by fetch-src.sh + prepare.sh.
#
# Run this on OPNsense / FreeBSD (it uses bmake, the FreeBSD toolchain and
# the host compiler).  It does NOT build a kernel: it produces the four
# loadable modules in config.mk KMODS (if_vce520b, vc_pcu_gpio, vc_board,
# vc_mdio) and nothing else. mdio.ko, etherswitch.ko, e6000sw.ko and ichwd.ko
# are stock OPNsense modules and are copied from the image by
# scripts/make-stock-usb.sh - rebuilding them here is how a box ends up
# running a .ko nobody verified.
#
#   usage: scripts/build.sh [-j N] [-k KERNDIR] [--clean] [--only "mods"]
#                           [--list] [--debug]
#
#     -k KERNDIR   directory holding the opt_*.h of the kernel you are
#                  injecting into. Without it the options files are
#                  synthesised from sys/<arch>/conf/DEFAULTS, exactly like an
#                  untied "make -C sys/modules" build - which is what a stock
#                  OPNsense kernel was built with, so that is the normal case.
#     --debug      keep debug symbols (no OBJCOPY --strip-debug) and do not
#                  strip the module, so kgdb/crash can read it.
# ---------------------------------------------------------------------------
set -eu

. "$(dirname -- "$0")/common.sh"

JOBS=$( (getconf NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) | head -n1 )
KERNDIR=${KERNBUILDDIR:-}
ONLY=${ONLY:-}
DO_CLEAN=0
VERBOSE=
DEBUG_FLAGS=

while [ $# -gt 0 ]; do
	case "$1" in
	-j)
		JOBS=$2
		shift 2
		;;
	-k)
		KERNDIR=$2
		shift 2
		;;
	--clean)
		DO_CLEAN=1
		shift
		;;
	--only)
		ONLY=$2
		shift 2
		;;
	--list)
		printf '%s\n' $KMODS
		exit 0
		;;
	--debug)
		DEBUG_FLAGS="DEBUG_FLAGS=-g"
		shift
		;;
	-d|--debug-make)
		VERBOSE="-d"
		shift
		;;
	-h|--help)
		sed -n '2,25p' "$0"
		exit 0
		;;
	*)
		die "unknown option '$1' (try --help)"
		;;
	esac
done

resolve_target

src_is_ready || die "kernel source tree missing - run scripts/fetch-src.sh first"

# ---------------------------------------------------------------------------
# Refuse to build against an unpatched tree.
# ---------------------------------------------------------------------------
grep -q VeloCloud "$SRC/sys/dev/e1000/if_em.c" 2>/dev/null ||
	die "the patch series is not applied - run scripts/prepare.sh first"

# ---------------------------------------------------------------------------
# The four module directories are not part of the OPNsense tree. Each is built
# in place, objects in $WORK/obj/<module> via MAKEOBJDIR.
# ---------------------------------------------------------------------------

if [ "$DO_CLEAN" = 1 ]; then
	log "cleaning $WORK/obj"
	rm -rf "$WORK/obj"
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.ko 2>/dev/null || true

# ---------------------------------------------------------------------------
# bmake plumbing.
#   -m $SRC/share/mk   the tree's own bsd.kmod.mk; also pins SRCTOP.
#   SYSDIR             $SRC/sys - autodetection is relative to .CURDIR, which is
#                      wrong out of tree.
#   VELOVC             this repo's src/, where the vc_* headers live.
#   NO_SKIP_MPATH      force the *_if.h generation (bus_if.h, mdio_if.h, ...).
#   MAKEOBJDIR         absolute per module, so the objdir is not derived from
#                      OBJROOT/OBJTOP.
# ---------------------------------------------------------------------------
MKARGS="
	-m $SRC/share/mk
	SRCTOP=$SRC
	SYSDIR=$SRC/sys
	VELOVC=$TOP/src
	NO_SKIP_MPATH=1
	$DEBUG_FLAGS
"

if [ -n "$KERNDIR" ]; then
	[ -d "$KERNDIR" ] || die "kernel build directory $KERNDIR does not exist"
	[ -f "$KERNDIR/opt_global.h" ] ||
		warn "$KERNDIR has no opt_global.h - KERNBUILDDIR may be wrong"
	log "using kernel options from $KERNDIR"
	MKARGS="$MKARGS KERNBUILDDIR=$KERNDIR"
fi

# Where each module's Makefile lives. All four of ours are in $TOP/src, built
# in place; anything else would come out of the src tree (there is nothing
# else - the stock mdio/etherswitch/e6000sw/ichwd .ko files are copied from the
# OPNsense image by scripts/make-stock-usb.sh, never built here).
module_dir() {
	case "$1" in
	vce520b|vc_pcu_gpio|vc_board|vc_mdio) printf '%s/%s' "$TOP/src" "$1" ;;
	*)                                    printf '%s/sys/modules/%s' "$SRC" "$1" ;;
	esac
}

# Module name (config.mk KMODS) -> artefact name produced by bmake (KMOD).
module_ko() {
	case "$1" in
	vce520b) printf 'if_vce520b.ko' ;;
	*)       printf '%s.ko' "$1" ;;
	esac
}

log "target: OPNsense $OPNSENSE_TARGET (src $SRC_BRANCH, FreeBSD $FREEBSD_VER, patch set $PATCHSET)"
log "objects: $WORK/obj   artefacts: $OUT   jobs: $JOBS"

built=
failed=
for m in $KMODS; do
	if [ -n "$ONLY" ]; then
		case " $ONLY " in
		*" $m "*) ;;
		*) continue ;;
		esac
	fi

	dir=$(module_dir "$m")
	[ -f "$dir/Makefile" ] || die "no Makefile for module '$m' in $dir"

	objdir="$WORK/obj/$m"
	mkdir -p "$objdir"
	ko=$(module_ko "$m")
	# Remove stale artefacts so the search below cannot pick up an older build.
	rm -f "$objdir/$ko" "$dir/$ko"

	log "building $m  ($dir -> $objdir)"
	# shellcheck disable=SC2086
	if [ "$DO_CLEAN" = 1 ]; then
		make -C "$dir" $MKARGS MAKEOBJDIR="$objdir" clean || true
	fi
	if ! make -C "$dir" $MKARGS MAKEOBJDIR="$objdir" -j "$JOBS" all; then
		failed="$failed $m"
		continue
	fi

	found=
	for cand in "$objdir/$ko" "$dir/$ko"; do
		[ -f "$cand" ] && { found="$cand"; break; }
	done
	[ -n "$found" ] || die "build of $m reported success but $ko was not found"

	cp "$found" "$OUT/$ko"
	built="$built $ko"
done

printf '\n'
if [ -n "$failed" ]; then
	printf '\033[1;31mFAILED:\033[0m%s\n' "$failed"
	printf 'the modules above did not compile; the ones that did are in %s\n' "$OUT"
	exit 1
fi

log "built:$built"
ls -l "$OUT"

cat <<EOF

Install paths:

  New media:   sh scripts/make-stock-usb.sh --with-kmods    # build host, root
  Running box: sftp the .ko plus scripts/install-kmods.sh, scripts/fs-window.sh
               and config.mk, then sh install-kmods.sh <dir>

EOF
