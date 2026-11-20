#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Install our four .ko on a RUNNING 520B - the module equivalent of the kernel
# swap in docs/STORAGE.md section 4. This is the update path: once the box is
# booted from an image built by make-stock-usb.sh, a new driver build lands
# here, not on a fresh stick.
#
# READ THIS BEFORE USING IT ON A SERIAL BASE. The serial image mounts root `ro`
# (docs/STORAGE.md 1), so rc unionfs-mounts boot conf etc home root usr var over
# tmpfs, and a write to /boot/modules here lands in RAM and is gone at reboot.
# This script detects that and refuses. To install modules on a serial boot you
# have to drop the union over /boot and make / writable, which is a mount-state
# change this script will not make for you - do it yourself, deliberately, or
# build new media with make-stock-usb.sh --with-kmods instead.
#
#   usage (on the box, over ssh):
#     sftp -b - -o BatchMode=yes root@<lease from `ifconfig igb4>` <<'EOF'
#     put if_vce520b.ko put vc_pcu_gpio.ko put vc_board.ko put vc_mdio.ko
#     put install-kmods.sh put config.mk
#     EOF
#     sh <dir>/install-kmods.sh <dir>
#     # <dir> = where those files landed; with no `cd` in the batch that is
#     # root's home on the box.
#
#   --rollback   restore the previous set from /vc-backup/kmods and exit
#   --no-reload  install and re-index, but do not attempt a live kldload
#   --force      skip the kernel-version match (you are then owning the ABI)
#   --grow-root  grow the root slice to the whole device. Needed on the serial
#                base: rc only grows when /.probe.for.growfs exists, and the
#                serial image does not ship it (STORAGE.md 2). Idempotent.
#
# This script does not change how the filesystem is handled. The base image
# decides that - the serial fstab this tree builds on is
# `/dev/ufs/OPNsense_Install / ufs ro,noatime 1 1`, root is read-only, and
# OPNsense's own rc deals with writes by unioning over tmpfs. Nothing here
# remounts, unmounts or re-flags anything. If / is not writable, this script
# stops and says so instead of working around it - because on this base
# "working around it" means writing into RAM and reporting success.
#
# Why each step is there:
#
#   /boot/modules, never /boot/kernel - that directory is what an OPNsense
#   update replaces, so anything of ours there is gone the first update.
#   Nothing in /boot/kernel is written, deleted or shadowed. See STORAGE.md 3.
#
#   Backup + two md5s + atomic rename + read back, exactly as for a kernel. A
#   .ko that half-copied is a boot failure on the next reboot, and the previous
#   set is the only cheap way back.
#
#   The version gate is not ceremony: a .ko is only safe against the exact
#   kernel it was built for. `uname -v` on the box prints the src commit; the
#   modules must have been built against that same commit. The full symbol
#   comparison lives on the offline side (make-stock-usb.sh, "Module ABI gate")
#   because OPNsense's base may not ship nm - if nm is present we repeat it
#   here, if not we say so rather than pretend.
#
#   kldxref is required: the loader resolves module dependencies from
#   linker.hints, and a stale linker.hints is how a correctly installed module
#   turns up "not found" at boot. Only /boot/modules is re-indexed - the stock
#   kernel's /boot/kernel/linker.hints is never rewritten.
#
#   Write discipline, and it is the only one here: stage the new file beside the
#   old one, chmod it, atomic rename on the same filesystem, sync, then read back
#   and compare md5s. A half-copied .ko is a boot failure, so the read-back is
#   not optional. A trap syncs even if the script dies partway.

set -eu

# This runs on the 520B, so it cannot source scripts/common.sh - that is a
# build-side file. It is self-contained and reads config.mk the same way
# common.sh does: config.mk is a bmake fragment whose `?=` assignments /bin/sh
# cannot parse, so the values are extracted with sed, never sourced.
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG_MK=""
for cand in "$PWD/config.mk" "$SELF/config.mk" "$SELF/../config.mk"; do
	[ -f "$cand" ] && { CONFIG_MK=$cand; break; }
done

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

_mkvar() {
	[ -n "$CONFIG_MK" ] || return 0
	sed -n "s/^$1[[:space:]]*[?]*=[[:space:]]*//p" "$CONFIG_MK" 2>/dev/null |
		sed -n '1p'
}

BOOT_KMODS=$(_mkvar BOOT_KMODS)
if [ -z "$BOOT_KMODS" ]; then
	BOOT_KMODS="vc_pcu_gpio vc_board vc_mdio vce520b"
	warn "no config.mk found - using the built-in module list"
fi

DIR=""
ROLLBACK=0
RELOAD=1
FORCE=0
GROW=0
for a in "$@"; do
	case "$a" in
	--rollback) ROLLBACK=1 ;;
	--no-reload) RELOAD=0 ;;
	--force) FORCE=1 ;;
	--grow-root) GROW=1 ;;
	-h|--help) sed -n '2,67p' "$0"; exit 0 ;;
	-*) die "unknown option: $a" ;;
	*) DIR=$a ;;
	esac
done
DIR=${DIR:-.}

BACKUP=/vc-backup/kmods
DEST=/boot/modules
CONF=/boot/loader.conf.d/vce520b-kmods.conf

# ko_base lives in make-stock-usb.sh; duplicated deliberately rather than
# sourcing that 23 KB script for one case statement.
ko_base() {
	case "$1" in
	vce520b) printf 'if_vce520b' ;;
	*) printf '%s' "$1" ;;
	esac
}

# --------------------------------------------------------------------------
# The mount state is the image's, not ours. Check it and stop if a write here
# would not reach the drive; never remount to make a write work.
#
# Two distinct failures on a serial base, and they need to be reported
# separately (docs/STORAGE.md 1):
#   - / is ro                 -> a write to a non-unioned path fails outright.
#   - the path is unioned     -> the write SUCCEEDS and goes to RAM. This is the
#     dangerous one: it looks like it worked. rc unions boot conf etc home root
#     usr var when it cannot create /.diskless, i.e. exactly when / is ro.
# So the check is not just "is / ro", it is "is the destination under a union".
# --------------------------------------------------------------------------
require_rw_root() {
	root_mount=$(mount | awk '$2 == "/"')
	case $root_mount in
	*read-only*)
		die "/ is mounted read-only - that is the serial base's fstab, and this
script does not remount it. Worse, rc has unioned boot conf etc home root usr var
over tmpfs, so a write to /boot/modules would succeed and land in RAM. Either
build new media (make-stock-usb.sh --with-kmods), or drop the union over the path
and make / writable yourself, deliberately: 'umount /boot' then 'mount -uw /'.
Verify with: mount | grep -E ' / |union'"
		;;
	esac
	if mount | awk '$3 == "unionfs" || /\(union/' | grep -q ' /boot\| /etc\| /usr'; then
		die "a union layer is mounted over /boot, /etc or /usr. A write there goes to
RAM, not the drive. This script will not write through it - umount the union
first, deliberately, or build new media instead."
	fi
	trap 'sync' EXIT HUP INT TERM
}

# --------------------------------------------------------------------------
# Grow the root slice. The serial image ships a 2.4 G UFS slice on a 15 G stick
# (or 7.3 G of internal flash). It does NOT grow itself: rc gates the grow on
# `[ -f ${GROWFS_MARKER} ]` and the serial image ships no such file - the string
# probe.for.growfs occurs exactly once in the whole image, as the GROWFS_MARKER=
# assignment. So --grow-root is the normal path on this base, not an exception.
# Idempotent: a slice that already fills the disk is left alone.
#
# Gotcha, measured (STORAGE.md 2): dd of a short image leaves the backup GPT at
# the end of the IMAGE, not of the disk, so gpart reports [CORRUPT] and resize
# fails. gpart recover first.
# --------------------------------------------------------------------------
if [ "$GROW" = 1 ]; then
	[ "$(id -u)" = 0 ] || die "run as root on the 520B"
	# Root is mounted by label (STORAGE.md 1), so resolve the label to a node and
	# split it into disk + slice index. Never guess a disk number.
	require_rw_root
	root_dev=$(mount | awk '/ on \/ \(ufs/ { print $1; exit }')
	[ -n "$root_dev" ] || die "cannot identify the root device from mount output"
	dev=$(readlink -f "$root_dev" 2>/dev/null || printf '%s' "$root_dev")
	# Three naming schemes, and which one applies is the base image's choice
	# (STORAGE.md 1). The disk node itself is whatever this host calls the drive, so
	# only the suffix is parsed: GPT says <disk>p4, MBR says <disk>s4, and a BSD
	# disklabel says <disk>a - one letter, no p or s at all. A BSD slice letter maps
	# to gpart's 1-based index: a=1, b=2, ...
	case $dev in
	*p[0-9]*) disk=${dev%p[0-9]*}; idx=${dev##*p} ;;
	*s[0-9]*) disk=${dev%s[0-9]*}; idx=${dev##*s} ;;
	*[a-h])
		disk=${dev%[a-h]}
		case ${dev##*[!a-z]} in
		a) idx=1 ;; b) idx=2 ;; c) idx=3 ;; d) idx=4 ;;
		e) idx=5 ;; f) idx=6 ;; g) idx=7 ;; h) idx=8 ;;
		*) die "cannot read the slice letter out of $dev" ;;
		esac
		;;
	*) die "cannot split $dev into disk and slice (expected <disk>pN, <disk>sN or
<disk><letter>)" ;;
	esac
	log "root slice $dev (index $idx) on $disk"
	command -v growfs >/dev/null || die "no growfs on this base - cannot grow"
	gpart show "$disk"
	gpart recover "$disk" || warn "gpart recover failed - continuing"
	gpart resize -i "$idx" "$disk" ||
		warn "gpart resize -i $idx $disk failed - already full?"
	# A stick that already grew lands here with the slice full, and growfs exits
	# non-zero saying so. That is the idempotent answer, not a failure -
	# distinguish it from a real growfs error.
	grow_rc=0
	grow_out=$(growfs -y "$dev" 2>&1) || grow_rc=$?
	if [ "$grow_rc" != 0 ]; then
		printf '%s\n' "$grow_out" | sed 's/^/    /'
		case $grow_out in
		*"entire partition"* | *"already"*)
			log "filesystem already fills $dev - nothing to grow"
			;;
		*) die "growfs failed on $dev" ;;
		esac
	fi
	sync
	trap - EXIT HUP INT TERM
	log "grown:"
	df -h /
	exit 0
fi

# --------------------------------------------------------------------------
# Rollback first - it must work when the install path is what broke.
# --------------------------------------------------------------------------
if [ "$ROLLBACK" = 1 ]; then
	[ "$(id -u)" = 0 ] || die "run as root on the 520B"
	[ -d "$BACKUP" ] || die "no $BACKUP - nothing to roll back to"
	require_rw_root
	mkdir -p "$DEST"
	for m in $BOOT_KMODS; do
		b=$(ko_base "$m")
		[ -f "$BACKUP/$b.ko" ] || continue
		cp "$BACKUP/$b.ko" "$DEST/$b.ko.new"
		chmod 0444 "$DEST/$b.ko.new"
		mv -f "$DEST/$b.ko.new" "$DEST/$b.ko"
		printf '  restored %s.ko  md5 %s\n' "$b" "$(md5 -q "$DEST/$b.ko")"
	done
	sync
	# Only /boot/modules is re-indexed: /boot/kernel/linker.hints belongs to the
	# stock kernel and is never rewritten by us.
	kldxref "$DEST"
	trap - EXIT HUP INT TERM
	log "rolled back from $BACKUP - reboot to take effect"
	exit 0
fi

# --------------------------------------------------------------------------
# Preflight: on the box, as root.
# --------------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "run as root on the 520B (this writes /boot/modules)"
uname -r | grep -qi opnsense ||
	die "this does not look like an OPNsense box - run it on the 520B, not on a
FreeBSD host used for building"
[ -d "$DEST" ] || die "no $DEST - this is not a booted OPNsense root"

# --------------------------------------------------------------------------
# Version gate.
# --------------------------------------------------------------------------
log "box kernel: $(uname -r) $(uname -v)"
[ -n "$CONFIG_MK" ] || warn "no config.mk next to this script - the version gate is
off. Put config.mk in the transfer directory to enable it."
anchor=$(_mkvar "ANCHOR_$(printf '%s' "opnsense_$(_mkvar OPNSENSE_TARGET)" | tr '.' '_')")
if [ "$FORCE" != 1 ] && [ -n "$anchor" ]; then
	# uname -v carries the ABBREVIATED commit (`stable/26.7-n283674-12334a596709`),
	# not the 40-char anchor, so compare on the prefix it actually prints.
	short=$(printf '%s' "$anchor" | cut -c1-12)
	case $(uname -v) in
	*"$short"*) log "kernel commit matches config.mk anchor $short" ;;
	*)
		die "the box runs a kernel from a different src commit than config.mk's
anchor ($anchor). Build the modules against the box's commit, or --force.
A mismatched .ko does not fail politely - it panics at load."
		;;
	esac
fi

# --------------------------------------------------------------------------
# Every file present before anything is written. A partial install is worse
# than no install: if_vce520b.ko needs vc_mdio.ko, which needs vc_pcu_gpio.ko.
# --------------------------------------------------------------------------
missing=""
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	[ -f "$DIR/$b.ko" ] || missing="$missing $b.ko"
done
[ -z "$missing" ] || die "not installing - missing from $DIR:$missing
Build them on a FreeBSD 15.1 host: sh scripts/build.sh -j 4, then sftp them
across."

log "installing from $DIR:"
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	printf '  %s.ko  %s bytes  md5 %s\n' "$b" \
		"$(stat -f %z "$DIR/$b.ko")" "$(md5 -q "$DIR/$b.ko")"
done

# Symbol check, if this base has nm. Strong undefined (`U`) only - weak ones
# (`w`) are legitimately unresolved until the kernel linker fills the set.
if command -v nm >/dev/null 2>&1; then
	kern_syms=$(mktemp)
	nm -g /boot/kernel/kernel 2>/dev/null |
		awk '$2 ~ /[TtDdBbRrVvWwi]/ { print $3 }' | sort -u >"$kern_syms"
	our_syms=$(mktemp)
	for m in $BOOT_KMODS; do
		nm "$DIR/$(ko_base "$m").ko" 2>/dev/null |
			awk '$2 !~ /[Uu]/ && $3 != "" { print $3 }' >>"$our_syms"
	done
	sort -u -o "$our_syms" "$our_syms"
	bad=0
	for m in $BOOT_KMODS; do
		f="$DIR/$(ko_base "$m").ko"
		gap=$(nm "$f" 2>/dev/null | awk '$2 == "U" { print $3 }' | sort -u |
			grep -v -x -F -f "$kern_syms" -f "$our_syms" || true)
		if [ -n "$gap" ]; then
			printf '  BAD  %s has unresolved symbols:\n' "$(ko_base "$m")"
			printf '%s\n' "$gap" | sed 's/^/         /'
			bad=1
		fi
	done
	rm -f "$kern_syms" "$our_syms"
	[ "$bad" = 0 ] || die "symbol check failed against the running kernel - not installing"
else
	warn "no nm on this base - symbol check skipped. It must have passed on the
the offline gate (scripts/make-stock-usb.sh, Module ABI gate) before these .ko
were copied over."
fi

# --------------------------------------------------------------------------
# Backup the outgoing set, with both md5s recorded.
# --------------------------------------------------------------------------
require_rw_root
mkdir -p "$BACKUP"
log "backing up the outgoing set -> $BACKUP"
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	if [ -f "$DEST/$b.ko" ]; then
		cp -p "$DEST/$b.ko" "$BACKUP/$b.ko"
		printf '  was  %s.ko  md5 %s\n' "$b" "$(md5 -q "$BACKUP/$b.ko")"
	else
		printf '  was  %s.ko  (absent - first install)\n' "$b"
	fi
done
printf '# replaced %s\n# incoming kernel: %s %s\n' "$(date)" "$(uname -r)" "$(uname -v)" \
	>>"$BACKUP/README"

# --------------------------------------------------------------------------
# Write: copy aside, atomic rename on the same filesystem, read back.
# --------------------------------------------------------------------------
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	cp "$DIR/$b.ko" "$DEST/$b.ko.new"
	chmod 0444 "$DEST/$b.ko.new"
	mv -f "$DEST/$b.ko.new" "$DEST/$b.ko"
done
sync
log "read back:"
fail=0
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	src=$(md5 -q "$DIR/$b.ko")
	dst=$(md5 -q "$DEST/$b.ko" 2>/dev/null || echo none)
	if [ "$src" = "$dst" ]; then
		printf '  ok   %s.ko  %s\n' "$b" "$dst"
	else
		printf '  BAD  %s.ko  want %s have %s\n' "$b" "$src" "$dst"
		fail=1
	fi
done
[ "$fail" = 0 ] || die "read-back mismatch - the write did not reach the drive.
Roll back with: sh $0 --rollback"

# --------------------------------------------------------------------------
# Load order, then the module index. loader.conf.d is read by the loader off the
# disk at boot, before rc runs, so this is what takes effect at reboot.
# --------------------------------------------------------------------------
mkdir -p "$(dirname "$CONF")"
{
	printf '# Generated by scripts/install-kmods.sh - stock kernel, our drivers\n'
	printf '# as modules in /boot/modules. Order matters: if_vce520b needs\n'
	printf '# vc_mdio_bus_read/write, which needs vc_gpio_pin_*.\n'
	for m in $BOOT_KMODS; do
		printf '%s_load="YES"\n' "$(ko_base "$m")"
	done
	# etherswitch/e6000sw are stock modules in /boot/kernel and are never touched
	# by us, but a stock kernel does not have them compiled in: if nothing names
	# them they never load, the mdio bus that igb creates has no driver
	# registered on it, and the LAN jacks stay dead - check with
	# `kldstat | grep -E 'e6000sw|etherswitch'` against `dmesg | grep -i mdio`.
	# They load before if_vce520b so the driver is registered when
	# igb attaches and creates the bus.
	printf 'etherswitch_load="YES"\n'
	printf 'e6000sw_load="YES"\n'
} >"$CONF"
log "wrote $CONF:"
sed 's/^/    /' "$CONF"

# Only /boot/modules: /boot/kernel/linker.hints belongs to the stock kernel and
# is never rewritten by us.
kldxref "$DEST" || die "kldxref failed - the loader will not find these"
log "linker.hints refreshed"

# Everything written has reached the drive (sync + md5 read back above), so the
# write path is closed here. The live smoke test below reads files, it does not
# write any.
sync
trap - EXIT HUP INT TERM

# --------------------------------------------------------------------------
# Live reload, best effort. Our modules are in use by the interfaces carrying
# ssh, so unloading them drops the network: this is a smoke test of the new
# files, not a swap. If it fails, the running kernel still has the old modules
# and the new ones are on disk for the next boot.
# --------------------------------------------------------------------------
if [ "$RELOAD" = 1 ]; then
	log "live smoke test (does not unload what is in use):"
	for m in $BOOT_KMODS; do
		b=$(ko_base "$m")
		if kldload "$DEST/$b.ko" 2>/dev/null; then
			printf '  loaded %s.ko\n' "$b"
		else
			printf '  in use / already loaded: %s.ko - takes effect at reboot\n' "$b"
		fi
	done
	kldstat | grep -E 'vc_|vce520b|igb' | sed 's/^/    /' || true
fi

cat <<EOF

Installed. Next:
  reboot                      # loader picks them up from $CONF
  dmesg | grep -E 'vc_|igb_vc|e6000sw|mdio'
  sh /usr/local/etc/rc.syshook.d/early/20-probe    # full dump on the console
Undo:
  sh $0 --rollback && reboot
EOF
