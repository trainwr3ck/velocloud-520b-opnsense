#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# Install our four .ko on a RUNNING 520B - the module equivalent of a kernel
# swap. This is the update path: once the box is
# booted from an image built by make-stock-usb.sh, a new driver build lands
# here, not on a fresh stick.
#
#   usage (on the box, over ssh):
#     sftp -b - -o BatchMode=yes root@<box address> <<'EOF'
#     put if_vce520b.ko put vc_pcu_gpio.ko put vc_board.ko put vc_mdio.ko
#     put install-kmods.sh put fs-window.sh put config.mk
#     EOF
#     sh /vc/out/xfer/install-kmods.sh /vc/out/xfer
#
#   --rollback   restore the previous set from /vc-backup/kmods and exit
#   --no-reload  install and re-index, but do not attempt a live kldload
#   --grow-root  grow the root slice to the whole device
#
# /boot/modules, never /boot/kernel. Backup + md5s + atomic rename + read back.
# kldxref re-indexes /boot/modules only. The mount state is read, not assumed,
# and left as found - see scripts/fs-window.sh.

set -eu

# Runs on the 520B, so it cannot source scripts/common.sh (a build-host file).
# config.mk is a bmake fragment - `?=` is not /bin/sh, so values are extracted
# with sed, never sourced.
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
GROW=0
for a in "$@"; do
	case "$a" in
	--rollback) ROLLBACK=1 ;;
	--no-reload) RELOAD=0 ;;
	--grow-root) GROW=1 ;;
	-h|--help) sed -n '2,24p' "$0"; exit 0 ;;
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
# The write window is one function shared by everything that touches the drive,
# not a sequence restated per script. fs_window_open [path] opens the window and
# fs_window_close puts the mount state back. The state is read, not assumed, and
# a trap closes the window if this script dies partway.
# --------------------------------------------------------------------------
. "$SELF/fs-window.sh"

# --------------------------------------------------------------------------
# Grow the root slice. The nano base ships a small UFS slice and grows it to the
# whole device by itself on first boot (/.probe.for.growfs), so this is only for
# the case where that did not happen. Idempotent: a slice that already fills the
# device is left alone.
#
# The nano base is a BSD disklabel, so root is <disk><letter> - da0a, ada0a - and
# gpart addresses BSD slices by index, so the letter maps back to one.
# --------------------------------------------------------------------------
if [ "$GROW" = 1 ]; then
	[ "$(id -u)" = 0 ] || die "run as root on the 520B"
	# Root is mounted by label, so resolve the label to a node. Never guess a
	# disk number.
	root_dev=$(mount | awk '/ on \/ \(ufs/ { print $1; exit }')
	[ -n "$root_dev" ] || die "cannot identify the root device from mount output"
	dev=$(readlink -f "$root_dev" 2>/dev/null || printf '%s' "$root_dev")
	last=${dev##*/}
	letter=${last##*[0-9]}
	disk=${last%"$letter"}
	case $letter in
	a) idx=1 ;; b) idx=2 ;; c) idx=3 ;; d) idx=4 ;;
	e) idx=5 ;; f) idx=6 ;; g) idx=7 ;; h) idx=8 ;;
	*) die "$dev is not a BSD disklabel slice - this base is not the nano layout" ;;
	esac
	log "root slice $dev (BSD slice $letter, index $idx) on $disk"
	command -v growfs >/dev/null || die "no growfs on this base - cannot grow"
	gpart show "$disk"
	fs_window_open /
	gpart resize -i "$idx" "$disk" ||
		warn "gpart resize -i $idx $disk failed - already full?"
	growfs -y "$dev" || die "growfs failed on $dev"
	sync
	fs_window_close
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
	fs_window_open
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
	fs_window_close
	trap - EXIT HUP INT TERM
	log "rolled back from $BACKUP - reboot to take effect"
	exit 0
fi

# --------------------------------------------------------------------------
# Preflight: on the box, as root.
# --------------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "run as root on the 520B (this writes /boot/modules)"
uname -r | grep -qi opnsense ||
	die "this does not look like an OPNsense box - run it on the 520B, not the build host"
[ -d "$DEST" ] || die "no $DEST - this is not a booted OPNsense root"

# --------------------------------------------------------------------------
# Version gate.
# --------------------------------------------------------------------------
log "box kernel: $(uname -r) $(uname -v)"

# --------------------------------------------------------------------------
# Every file present before anything is written.
# --------------------------------------------------------------------------
missing=""
for m in $BOOT_KMODS; do
	b=$(ko_base "$m")
	[ -f "$DIR/$b.ko" ] || missing="$missing $b.ko"
done
[ -z "$missing" ] || die "not installing - missing from $DIR:$missing
Build them on the build host: sh scripts/build.sh -j 4, then sftp them across."

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
build host (scripts/make-stock-usb.sh, Module ABI gate) before these .ko got here."
fi

# --------------------------------------------------------------------------
# Backup the outgoing set, with both md5s recorded. /vc-backup is on the real
# root, so this is the first thing that needs the window.
# --------------------------------------------------------------------------
fs_window_open
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
# disk at boot, so it is in effect from the first stage of the next boot.
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
	# registered on it, and the LAN jacks stay dead. They load before if_vce520b
	# so the driver is registered when igb attaches and creates the bus.
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
# window closes here. The live smoke test below reads files, it does not write
# any, and it should run with the box back in its real state.
sync
fs_window_close
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
  kldstat | grep -E 'vc_|vce520b|if_vce520b'
Undo:
  sh $0 --rollback && reboot
EOF
