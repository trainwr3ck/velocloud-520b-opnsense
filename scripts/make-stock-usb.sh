#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# VeloCloud Edge 520B image: stock OPNsense kernel + our code as loadable modules.
#
#   scripts/make-stock-usb.sh                     # -> out/vce520b-stock.img
#   scripts/make-stock-usb.sh --image X.img       # graft onto another image
#   scripts/make-stock-usb.sh --no-probe          # skip the boot diagnostic dump
#   scripts/make-stock-usb.sh --mode disable      # disarm the dog instead of petting
#   scripts/make-stock-usb.sh --with-kmods        # install out/*.ko into /boot/modules
#   scripts/make-stock-usb.sh --verify            # inspect an existing image, change nothing
#
# --with-kmods is the DELIVERY SWITCH: the kernel stays stock OPNsense, so
# `opnsense-update` can never strand the box on a kernel it did not build, and
# everything of ours ships as loadable modules in /boot/modules. It refuses to
# run unless out/*.ko were built against the same src commit the stock image's
# kernel came from - a module built against another tree is a boot failure, not
# a warning. See "Module ABI" below.
#
# mdio.ko, etherswitch.ko, e6000sw.ko and ichwd.ko are NOT rebuilt here. They
# are stock OPNsense modules, used as shipped; only the four in config.mk
# BOOT_KMODS are ours. See README, "Why modules, and what it costs".
#
# What it changes on the target root (kernel and stock modules are left ALONE):
#   boot/loader.conf.d/vce520b-console.conf  serial console on UART1 @ 0x2F8
#   boot/loader.conf.d/vce520b-wdt.conf      ichwd_load="YES", bootverbose
#   boot/loader.conf.d/vce520b-kmods.conf    module load order (--with-kmods)
#   boot/loader.conf.d/vce520b-switch.conf   e6000sw device hints
#   etc/rc.conf.d/watchdogd                  pet the dog
#   usr/local/etc/vce520b-wdt.conf           feeder policy
#   usr/local/etc/vce520b-switch.conf/.spec  switch port/VLAN bring-up
#   usr/local/etc/rc.syshook.d/early/{00-watchdog,10-switch,20-probe}
#
# REQUIRES ROOT on the FreeBSD host doing the grafting: mdconfig and mount are
# privileged. Any FreeBSD host with the image file will do - nothing here is
# specific to one machine.

set -eu
. "$(dirname "$0")/common.sh"

STOCK_IMG=""
USE_PROBE=1
WDT_MODE=service
VERIFY=0
WITH_KMODS=0
NANO=0
MNT="$WORK/mnt"

while [ $# -gt 0 ]; do
	case "$1" in
	--image) STOCK_IMG=$2; shift ;;
	--no-probe) USE_PROBE=0 ;;
	--mode) WDT_MODE=$2; shift ;;
	--with-kmods) WITH_KMODS=1 ;;
	--verify) VERIFY=1 ;;
	-h|--help) sed -n '2,37p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	# Flag-only options (--no-probe, --verify) do not shift themselves. Without
	# this the loop spins forever with $# unchanged: `--verify` looked like a
	# hung build and produced zero output for ten minutes.
	shift
done

case "$WDT_MODE" in
service | disable) ;;
*) die "--mode must be 'service' or 'disable', got: $WDT_MODE" ;;
esac

# mdconfig/mount fail with a confusing error deep in the script; check up front.
[ "$(id -u)" = 0 ] || die \
"this script needs root on the FreeBSD host it runs on (mdconfig + mount):
  doas sh scripts/make-stock-usb.sh     # or sudo, or log in as root"

resolve_target
mkdir -p "$TOP/out"

# --------------------------------------------------------------------------
# Module ABI gate (--with-kmods).
#
# A loadable module is only safe if every symbol it leaves undefined is
# resolvable in the kernel it is about to be booted against. That is not a
# judgement call, it is a set comparison, so do it here rather than finding it
# out from a kernel panic on the box.
#
#   OUR_KMODS - what we install into /boot/modules, in load order
#   (config.mk BOOT_KMODS). Every undefined symbol in each must appear in the
#   stock kernel's symbol table, or in the defined symbols of another module
#   in the set.
#
# `nm -u` on a .ko lists undefined symbols; `nm -g` on the kernel lists what it
# exports. Both are cheap.
# --------------------------------------------------------------------------
OUR_KMODS="$BOOT_KMODS"

# config.mk module name -> .ko basename. Only vce520b differs: the file is
# if_vce520b.ko. The loader line names the file; the driver name registered
# inside it is still `igb`, which is what keeps the interfaces igb0..igb5.
ko_base() {
	case "$1" in
	vce520b) printf 'if_vce520b' ;;
	*)       printf '%s' "$1" ;;
	esac
}

check_kmod_abi() {
	kernel="$1"
	dir="$2"
	# Symbols the kernel itself exports. `nm -g` is right for a kernel: it is a
	# normal ELF and the kernel linker resolves modules against its global
	# symbols. The type set must include `i` (STT_GNU_IFUNC) - memcpy and memset
	# are ifuncs in the 26.7 kernel, and dropping that type made every module
	# look like it had unresolved memcpy/memset.
	kern_syms=$(mktemp)
	nm -g "$kernel" 2>/dev/null | awk '$2 ~ /[TtDdBbRrVvWwi]/ { print $3 }' |
		sort -u >"$kern_syms"
	# Symbols our other modules export. Here `nm -g` is WRONG and was the
	# original false failure: bsd.kmod.mk runs kmod_syms.awk + objcopy, which
	# marks everything not in EXPORT_SYMS local (`-L`), so a loadable module
	# legitimately has almost no GLOBAL symbols - MEASURED: `nm -g` on the
	# stock 26.7 /boot/kernel/etherswitch.ko prints zero, yet e6000sw.ko
	# resolves etherswitch_readreg_desc from it fine. The kernel linker reads
	# the whole .symtab of a loaded module, so compare against `nm` (all
	# bindings) and only exclude the undefined ones.
	our_syms=$(mktemp)
	for m in $OUR_KMODS; do
		f="$dir/$(ko_base "$m").ko"
		[ -f "$f" ] || continue
		nm "$f" 2>/dev/null |
			awk '$2 !~ /[Uu]/ && $3 != "" { print $3 }' >>"$our_syms"
	done
	sort -u -o "$our_syms" "$our_syms"

	bad=0
	for m in $OUR_KMODS; do
		f="$dir/$(ko_base "$m").ko"
		[ -f "$f" ] || { printf '  MISS %s.ko not built\n' "$(ko_base "$m")"; bad=1; continue; }
		# Only STRONG undefined (`U`) symbols must resolve. `nm -u` also lists
		# weak ones (`w __start_set_sysinit_set` and friends), and those are
		# legitimately absent until the kernel linker fills the set - treating
		# them as failures is a false alarm.
		missing=$(nm "$f" 2>/dev/null | awk '$2 == "U" { print $3 }' |
			sort -u | grep -v -x -F -f "$kern_syms" -f "$our_syms" || true)
		if [ -n "$missing" ]; then
			printf '  BAD  %s.ko has unresolved symbols:\n' "$(ko_base "$m")"
			printf '%s\n' "$missing" | sed 's/^/         /'
			bad=1
		else
			printf '  ok   %s.ko  %s bytes, all undefined symbols resolve\n' \
				"$(ko_base "$m")" "$(stat -f %z "$f")"
		fi
	done
	rm -f "$kern_syms" "$our_syms"
	[ "$bad" = 0 ] || die "module ABI check failed - the .ko in $dir were not built against this kernel"
}

# --------------------------------------------------------------------------
# --verify: attach the image we already built and prove every file landed.
# Kept inside this script on purpose rather than as a second script: it needs
# the same root-only mdconfig/mount access and the same file lists, and a
# second copy of those lists is a second thing to forget to update.
# --------------------------------------------------------------------------
if [ "$VERIFY" = 1 ]; then
	IMG="$TOP/out/vce520b-stock.img"
	[ -f "$IMG" ] || die "no $IMG - build it first"
	log "verifying $IMG ($(stat -f %z "$IMG") bytes)"
	DEV=$(mdconfig -f "$IMG")
	trap 'umount "$MNT" 2>/dev/null || true; mdconfig -d -u "${DEV#md}" 2>/dev/null || true' EXIT
	UFS=""
	for pfx in p s; do
		for s in 1 2 3 4 5 6 7 8; do
			node="/dev/${DEV}${pfx}${s}"
			[ -e "$node" ] || continue
			if fstyp "$node" 2>/dev/null | grep -qi ufs; then UFS="$node"; break 2; fi
		done
	done
	if [ -z "$UFS" ]; then
		for s in a b c d e f g h; do
			node="/dev/${DEV}${s}"
			[ -e "$node" ] || continue
			if fstyp "$node" 2>/dev/null | grep -qi ufs; then UFS="$node"; break; fi
		done
	fi
	[ -n "$UFS" ] || die "no UFS partition in $IMG"
	mkdir -p "$MNT"
	mount -o ro "$UFS" "$MNT"

	fail=0
	for f in boot/loader.conf.d/vce520b-console.conf \
		boot/loader.conf.d/vce520b-wdt.conf \
		etc/rc.conf.d/watchdogd \
		usr/local/etc/vce520b-wdt.conf \
		usr/local/etc/rc.syshook.d/early/00-watchdog \
		usr/local/etc/rc.syshook.d/early/20-probe; do
		if [ -f "$MNT/$f" ]; then
			printf '  ok   %s  %s\n' "$f" "$(stat -f '%Sp %z' "$MNT/$f")"
		else
			printf '  MISS %s\n' "$f"
			fail=1
		fi
	done

	# The --with-kmods files are conditional, so they are checked only when the
	# delivery switch was used - a bare probe image legitimately has none of them.
	if [ -f "$MNT/boot/loader.conf.d/vce520b-kmods.conf" ]; then
		for f in boot/loader.conf.d/vce520b-switch.conf \
			usr/local/etc/vce520b-switch.conf \
			usr/local/etc/vce520b-switch.spec \
			usr/local/etc/rc.syshook.d/early/10-switch; do
			if [ -f "$MNT/$f" ]; then
				printf '  ok   %s  %s\n' "$f" "$(stat -f '%Sp %z' "$MNT/$f")"
			else
				printf '  MISS %s\n' "$f"
				fail=1
			fi
		done
	fi

	log "console settings actually in the image:"
	grep -h -E 'console|uart' "$MNT/boot/loader.conf.d/vce520b-console.conf" \
		"$MNT/boot/loader.conf" 2>/dev/null | grep -v '^#' | sed 's/^/    /'

	log "watchdog policy:"
	cat "$MNT/etc/rc.conf.d/watchdogd" 2>/dev/null | sed 's/^/    /'
	cat "$MNT/usr/local/etc/vce520b-wdt.conf" 2>/dev/null | grep -v '^#' | sed 's/^/    /'

	log "stock modules we depend on (must be the OPNsense originals):"
	for k in $STOCK_KMODS; do
		found=$(ls "$MNT"/boot/kernel/${k}.ko 2>/dev/null | head -1)
		if [ -n "$found" ]; then
			printf '  ok   boot/kernel/%s.ko\n' "$k"
		else
			printf '  MISS %s.ko\n' "$k"
			fail=1
		fi
	done

	log "stock modules must NOT be replaced by ours:"
	for k in if_em if_igb; do
		if [ -f "$MNT/boot/modules/$k.ko" ]; then
			printf '  BAD  boot/modules/%s.ko - our module is if_vce520b.ko, not this\n' "$k"
			fail=1
		else
			printf '  ok   no boot/modules/%s.ko\n' "$k"
		fi
	done

	log "kernel must be STOCK (no igb_vc):"
	if strings -n 12 "$MNT/boot/kernel/kernel" 2>/dev/null | grep -q 'igb_vc: func' ||
		gzip -dc "$MNT/boot/kernel/kernel" 2>/dev/null | strings -n 12 | grep -q 'igb_vc: func'; then
		printf '  BAD  kernel contains igb_vc - this is not a stock test\n'
		fail=1
	else
		printf '  ok   kernel is stock, %s bytes\n' "$(stat -f %z "$MNT/boot/kernel/kernel")"
	fi

	log "our code:"
	# Two legitimate shapes. A bare image (no --with-kmods) must have none of our
	# modules; a --with-kmods image must have all four, and then the ABI gate is
	# re-run against the kernel actually inside the image.
	ours=$(ls "$MNT"/boot/kernel/vc_*.ko "$MNT"/boot/kernel/if_vce520b.ko \
		"$MNT"/boot/modules/vc_*.ko "$MNT"/boot/modules/if_vce520b.ko 2>/dev/null || true)
	if [ ! -f "$MNT/boot/loader.conf.d/vce520b-kmods.conf" ]; then
		if [ -n "$ours" ]; then
			printf '  BAD  %s\n' "$ours"
			fail=1
		else
			printf '  ok   no vc_* / if_vce520b modules - kernel + modules pure stock\n'
		fi
	else
		printf '  ok   module delivery (vce520b-kmods.conf present)\n'
		for m in $OUR_KMODS; do
			b=$(ko_base "$m")
			if [ -f "$MNT/boot/modules/$b.ko" ]; then
				printf '  ok   boot/modules/%s.ko  %s\n' "$b" \
					"$(stat -f %z "$MNT/boot/modules/$b.ko")"
			else
				printf '  MISS boot/modules/%s.ko\n' "$b"
				fail=1
			fi
		done
		log "re-running the ABI gate against the kernel inside the image:"
		check_kmod_abi "$MNT/boot/kernel/kernel" "$MNT/boot/modules"
	fi

	umount "$MNT"
	mdconfig -d -u "${DEV#md}"
	trap - EXIT
	[ "$fail" = 0 ] && log "VERIFY OK - image is ready to flash" ||
		die "VERIFY FAILED - something is missing"
	exit 0
fi

# 26.7 has no "memstick" flavour: serial / vga / nano / dvd.
#
# "serial" is the base for the 520B: GPT + UFS, root mounted ro by label, and it
# boots headless on a serial console - the only console the board exposes, since
# VeloCloud ships no serial port on the rear panel (README.md, "The hardware").
# "nano" (embedded, BSD disklabel, rw root) is accepted too if you hand us one
# with --image, but it is not what this tree builds against.
if [ -z "$STOCK_IMG" ]; then
	STOCK_IMG=$(ls "$TOP"/out/OPNsense-*-serial-amd64.img \
		"$TOP"/out/OPNsense-*-nano-amd64.img \
		"$TOP"/out/OPNsense-*-memstick.img 2>/dev/null |
		head -1 || true)
fi
[ -n "$STOCK_IMG" ] && [ -f "$STOCK_IMG" ] || die \
"no stock image in out/. Fetch it:
  fetch -o out/OPNsense-${OPNSENSE_TARGET}-serial-amd64.img.bz2 \\
    https://pkg.opnsense.org/releases/${OPNSENSE_TARGET}/OPNsense-${OPNSENSE_TARGET}-serial-amd64.img.bz2
  bunzip2 out/OPNsense-${OPNSENSE_TARGET}-serial-amd64.img.bz2"

IMG="$TOP/out/vce520b-stock.img"
log "base: $STOCK_IMG"
# The base is the serial flavour. Read out of OPNsense-26.7-serial-amd64.img
# itself, not inferred:
#   - GPT, four slices; root is partition 4, UFS, labelled OPNsense_Install.
#   - /etc/fstab: `/dev/ufs/OPNsense_Install / ufs ro,noatime 1 1`. Root is ro at
#     rest, and rc responds by unionfs-mounting boot conf etc home root usr var
#     over tmpfs - the box behaves as live media (docs/STORAGE.md 1). This script
#     does not rewrite fstab on any base: how the image handles its filesystem is
#     the image's business. That is also why our modules must be grafted in here,
#     offline - a runtime write to a unioned path lands in RAM.
#   - /.probe.for.growfs is NOT shipped. rc gates the grow on
#     `[ -f ${GROWFS_MARKER} ]`, and that string occurs exactly once in the whole
#     image (the assignment), so a stock serial boot never grows the slice.
#     install-kmods.sh --grow-root is the one-time grow.
# Only the 520B-specific parts are added: UART1 @ 0x2F8, watchdog, modules,
# switch hints.
case "$STOCK_IMG" in
*-nano-*) NANO=1 ;;
esac
if [ "$NANO" = 1 ]; then
	log "flavour: nano (embedded, BSD label, rw root) - storage left as shipped"
else
	log "flavour: serial (GPT, ro root) - the base this tree builds against"
fi
log "copying -> $IMG"
cp "$STOCK_IMG" "$IMG"

# --------------------------------------------------------------------------
# Attach and find the UFS partition.
#
# CRITICAL: the serial image is GPT, and FreeBSD names GPT partitions
# `<disk>p1`, NOT `<disk>s1`. The `s` suffix is MBR-only, so probing
# ${DEV}s<N> silently matches nothing and the script dies with "no UFS
# partition found". Probe both.
#
# The serial base's layout is four slices - efi, freebsd-boot, freebsd-swap,
# freebsd-ufs - so the root is partition 4, NOT partition 2, and it is UFS not
# ZFS. Read it rather than trusting this comment:
#   DEV=$(mdconfig -f out/OPNsense-<ver>-serial-amd64.img); gpart show /dev/$DEV
# The offsets move between releases; the shape (root = the UFS slice, found by
# fstyp, not by index) is what this loop relies on.
# Also note `fstyp` prints the type in LOWERCASE - grep -qi, never grep -q UFS,
# which silently matches nothing and looks like a missing partition.
# Keep probing rather than assuming an index - it differs between flavours,
# and so does the naming SCHEME: GPT says <disk>p4, MBR says <disk>s1, and a
# BSD disklabel says <disk>a. Probe all three.
# --------------------------------------------------------------------------
log "attaching $IMG"
DEV=$(mdconfig -f "$IMG")
trap 'umount "$MNT" 2>/dev/null || true; mdconfig -d -u "${DEV#md}" 2>/dev/null || true' EXIT

UFS=""
for pfx in p s; do
	for s in 1 2 3 4 5 6 7 8; do
		node="/dev/${DEV}${pfx}${s}"
		# FreeBSD devfs exposes disk and partition nodes as CHARACTER devices
		# (crw-r-----), so `[ -b ]` is false for every mdXpN and the loop
		# silently skips them all. Test existence, not block-ness.
		[ -e "$node" ] || continue
		if fstyp "$node" 2>/dev/null | grep -qi ufs; then
			UFS="$node"
			break 2
		fi
	done
done
# The serial base is GPT and is found by the loop above. This second loop is for
# a base that is NOT GPT: the nano flavour is a BSD disklabel (`gpart show` on
# it -> one freebsd-ufs slice), so the kernel names the slice by letter -
# ${DEV}a - and there is no ${DEV}p1 or ${DEV}s1 at all. Without this loop such
# a base looks like it contains no UFS. Kept so --image works on any flavour.
if [ -z "$UFS" ]; then
	for s in a b c d e f g h; do
		node="/dev/${DEV}${s}"
		[ -e "$node" ] || continue
		if fstyp "$node" 2>/dev/null | grep -qi ufs; then
			UFS="$node"
			break
		fi
	done
fi
if [ -z "$UFS" ]; then
	log "what the kernel actually sees on $DEV:"
	ls "/dev/${DEV}"* 2>&1 | sed 's/^/    /'
	gpart show "/dev/$DEV" 2>&1 | sed 's/^/    /'
	die "no UFS partition found in $IMG"
fi
log "UFS partition: $UFS"

# A UFS slice copied out of a .img can carry the clean flag unset, and then an rw
# mount is refused outright:
#   mount: /dev/<node>: R/W mount ... denied. Filesystem is not clean - run fsck.
# fsck the copy in the image, never the shipped base. This is safe on a file:
# nothing is mounted there yet.
if fsck -y "$UFS" >/dev/null 2>&1; then
	log "fsck clean"
else
	log "fsck reported repairs on the image copy"
fi

mkdir -p "$MNT"
mount -o rw "$UFS" "$MNT"

# Prove the kernel really is stock - if a previous run grafted ours in, this
# image is not a stock test and the results are meaningless. A stock kernel is a
# PLAIN ELF (`head -c 4 | od -An -tx1` -> 7f454c46), so the raw strings test is
# the one that matters; the gzip test is kept only to catch an image built by an
# older script that gzipped our kernel. Do not compare sizes - a stock kernel's
# size changes with every release.
if strings -n 12 "$MNT/boot/kernel/kernel" 2>/dev/null |
	grep -q 'igb_vc: func'; then
	die "the kernel in $IMG is OURS, not stock. Delete $IMG and re-run."
fi
if gzip -dc "$MNT/boot/kernel/kernel" 2>/dev/null |
	strings -n 12 | grep -q 'igb_vc: func'; then
	die "the kernel in $IMG is OURS, not stock. Delete $IMG and re-run."
fi
log "kernel is stock - good"

# --------------------------------------------------------------------------
# Console first. The 520B's console is UART1 @ 0x2F8; the stock serial image
# assumes com1 @ 0x3F8. Get this wrong and the box looks dead.
# --------------------------------------------------------------------------
mkdir -p "$MNT/boot/loader.conf.d" "$MNT/etc/rc.conf.d" \
	"$MNT/usr/local/etc" "$MNT/usr/local/etc/rc.syshook.d/early"

# --------------------------------------------------------------------------
# --with-kmods: our drivers as loadable modules on a stock kernel.
#
# Load order is not optional. if_vce520b.ko's igb_vc code calls
# vc_mdio_bus_read/write (measured: `nm -u out/if_vce520b.ko` ->
# U vc_mdio_bus_read, U vc_mdio_bus_write), and vc_mdio.ko in turn needs
# vc_gpio_pin_* from vc_pcu_gpio.ko. kldload resolves dependencies from the
# file's own dependency list, so listing the leaves is enough - but being
# explicit costs nothing and is readable in loader.conf.
#
# They go in /boot/modules, NOT /boot/kernel: /boot/kernel is what
# `opnsense-update` replaces when it ships a kernel, and anything of ours
# living there is gone the first time the box updates itself. Nothing in
# /boot/kernel is deleted either - stock mdio/etherswitch/e6000sw/ichwd are
# used as shipped, and shadowing them is how a box ends up running a .ko
# nobody built.
# --------------------------------------------------------------------------
if [ "$WITH_KMODS" = 1 ]; then
	for m in $OUR_KMODS; do
		[ -f "$TOP/out/$(ko_base "$m").ko" ] || die \
"out/$(ko_base "$m").ko is missing - build the modules first:
  sh scripts/build.sh -j 4
against the src tree at the same commit the stock image's kernel came from."
	done
	log "module ABI vs the kernel actually in the image:"
	check_kmod_abi "$MNT/boot/kernel/kernel" "$TOP/out"
	mkdir -p "$MNT/boot/modules"
	for m in $OUR_KMODS; do
		b=$(ko_base "$m")
		rm -f "$MNT/boot/modules/$b.ko"
		cp "$TOP/out/$b.ko" "$MNT/boot/modules/$b.ko"
		chmod 0444 "$MNT/boot/modules/$b.ko"
		printf '  installed boot/modules/%s.ko\n' "$b"
	done
	kldxref "$MNT/boot/kernel" "$MNT/boot/modules" 2>/dev/null || true
fi

cat > "$MNT/boot/loader.conf.d/vce520b-console.conf" <<'EOF'
# VeloCloud Edge 520B: console is UART1 @ 0x2F8 (Linux ttyS1), not com1.
comconsole_port="0x2F8"
comconsole_speed="115200"
# Kernel-side console selection is separate from the loader's: uart0 is
# disabled, uart1 carries the console.
hint.uart.0.flags="0x0"
hint.uart.1.flags="0x10"
EOF
# Which console device to use is the base image's business, not ours. The serial
# base assumes com1 @ 0x3F8 and has no usable video console on this board, so on
# it we pin comconsole. A nano base instead ships console="comconsole,
# vidconsole" with boot_serial/boot_multicons in loader.conf and
# `-S115200 -h -D` in boot.config - serial primary, VGA secondary - and forcing
# comconsole-only there would throw the VGA console away. Skip it there.
if [ "$NANO" = 0 ]; then
	printf 'console="comconsole"\n' \
		>> "$MNT/boot/loader.conf.d/vce520b-console.conf"
fi

# bootverbose is what makes this run worth doing: it is what prints the I354
# function enumeration, any mdio bus creation, and e6000sw's "PHY at port N".
cat > "$MNT/boot/loader.conf.d/vce520b-wdt.conf" <<'EOF'
# Stock FreeBSD driver for the Avoton/Rangeley SoC watchdog (sys/dev/ichwd).
# ichwd_attach() calls ichwd_tmr_disable() during attach, which halts the
# timer the firmware armed at reset. That is the whole point of loading it.
ichwd_load="YES"
# Device enumeration detail for the bring-up probe. Costs a few seconds.
bootverbose="YES"
EOF

# --------------------------------------------------------------------------
# Watchdog petting. Same policy and same feeder script the shipping image
# uses, so this test exercises real code rather than a throwaway.
# --------------------------------------------------------------------------
cat > "$MNT/etc/rc.conf.d/watchdogd" <<'EOF'
watchdogd_enable="YES"
# 120s -> 2^37 ns -> ichwd sets 137 TCOv3 ticks = a 137s hardware deadline.
# Never ask for 180: that silently becomes 2^38 ns = 274s.
watchdogd_flags="-t 120 -s 30 -w -T 20 --pretimeout 45 --pretimeout-action log,printf"
EOF

cat > "$MNT/usr/local/etc/vce520b-wdt.conf" <<EOF
# Generated by scripts/make-stock-usb.sh
MODE=$WDT_MODE
PERIOD=120
NAP=30
EOF

if [ -f "$TOP/overlay/usr/local/etc/rc.syshook.d/early/00-watchdog" ]; then
	cp "$TOP/overlay/usr/local/etc/rc.syshook.d/early/00-watchdog" \
		"$MNT/usr/local/etc/rc.syshook.d/early/00-watchdog"
	chmod 0755 "$MNT/usr/local/etc/rc.syshook.d/early/00-watchdog"
else
	warn "overlay 00-watchdog missing - only watchdogd will pet the dog"
fi

# --------------------------------------------------------------------------
# The diagnostic dump. Runs as an early syshook so it prints before the ~3
# minute deadline even if everything else fails, and goes to the serial
# console where it can be copied out of the terminal.
# --------------------------------------------------------------------------
if [ "$USE_PROBE" = 1 ]; then
	if [ -f "$TOP/overlay/usr/local/etc/rc.syshook.d/early/20-probe" ]; then
		cp "$TOP/overlay/usr/local/etc/rc.syshook.d/early/20-probe" \
			"$MNT/usr/local/etc/rc.syshook.d/early/20-probe"
		chmod 0755 "$MNT/usr/local/etc/rc.syshook.d/early/20-probe"
		log "boot diagnostic dump installed"
	else
		warn "overlay 20-probe missing - no boot dump will be printed"
	fi
fi

# --------------------------------------------------------------------------
# Module load order + switch hints, only for the module-delivery image.
#
# The e6000sw device hints have no other home on a stock kernel: there is no
# compiled-in hint table to fall back on, so loader.conf.d is the only place
# they can come from. overlay/boot/loader.conf.d/e6000sw.conf is that single
# source of truth.
#
# ichwd_load stays YES: a stock kernel has the SoC watchdog only as a module.
# --------------------------------------------------------------------------
if [ "$WITH_KMODS" = 1 ]; then
	{
		printf '# Generated by scripts/make-stock-usb.sh --with-kmods\n'
		printf '# Stock kernel, our drivers as modules in /boot/modules.\n'
		printf '# Leaves first: if_vce520b references vc_mdio_bus_read/write,\n'
		printf '# which vc_mdio provides on top of vc_pcu_gpio. kldload pulls the\n'
		printf '# dependency itself; the explicit lines document the order.\n'
		for m in $OUR_KMODS; do
			printf '%s_load="YES"\n' "$(ko_base "$m")"
		done
		# etherswitch and e6000sw are STOCK modules and stay in /boot/kernel, but a
		# stock kernel does not have them compiled in, so nothing loads them unless
		# we say so. Without these two lines the
		# mdio bus is created under igb and then nothing probes it - dmesg shows
		# "mdio0: <MDIO> on igb2" and no e6000sw at all, and the LAN jacks are dead.
		# Reproduce it on any 520B: drop these lines, boot, and `kldstat` has no
		# e6000sw while `dmesg | grep -i mdio` still shows the bus.
		# Loading them here, before if_vce520b, means the driver is registered when
		# igb attaches and creates the bus.
		printf 'etherswitch_load="YES"\n'
		printf 'e6000sw_load="YES"\n'
	} >"$MNT/boot/loader.conf.d/vce520b-kmods.conf"
	# The e6000sw device hints. Same values the compiled-in hints use, read
	# from the repo so there is one source of truth.
	if [ -f "$TOP/overlay/boot/loader.conf.d/e6000sw.conf" ]; then
		cp "$TOP/overlay/boot/loader.conf.d/e6000sw.conf" \
			"$MNT/boot/loader.conf.d/vce520b-switch.conf"
	else
		warn "overlay e6000sw.conf missing - the switches will not attach"
	fi
	# Switch VLAN/port bring-up, the same hooks the shipping image installs.
	for f in 10-switch; do
		if [ -f "$TOP/overlay/usr/local/etc/rc.syshook.d/early/$f" ]; then
			cp "$TOP/overlay/usr/local/etc/rc.syshook.d/early/$f" \
				"$MNT/usr/local/etc/rc.syshook.d/early/$f"
			chmod 0755 "$MNT/usr/local/etc/rc.syshook.d/early/$f"
		fi
	done
	for f in vce520b-switch.conf vce520b-switch.spec; do
		[ -f "$TOP/overlay/usr/local/etc/$f" ] &&
			cp "$TOP/overlay/usr/local/etc/$f" "$MNT/usr/local/etc/"
	done
	# Storage: nothing. fstab, tmpfs and the grow marker are left exactly as the
	# base image ships them - how OPNsense handles its filesystem is the image's
	# decision, and rewriting it is not ours to make. On the serial base that means
	# root stays `ro,noatime` and rc unions boot conf etc home root usr var over
	# tmpfs (docs/STORAGE.md 1). Our modules are grafted in here, offline, precisely
	# because a runtime write to those paths would land in RAM.
	log "fstab root line, as shipped: $(grep -m1 ' / ' "$MNT/etc/fstab")"
	if [ -e "$MNT/.probe.for.growfs" ]; then
		log "growfs marker present - root grows to the whole stick on first boot"
	else
		warn "no /.probe.for.growfs in this base (normal for serial - rc only grows
if the marker exists, and the serial image does not ship it). The root slice stays
at its shipped 2.4 G. Grow it once on the box: sh install-kmods.sh --grow-root."
	fi
fi

# Belt for the serial base, which assumes com1 @ 0x3F8. A nano base already has
# all of this (loader.conf + boot.config), and appending console="comconsole"
# there would replace its comconsole,vidconsole pair. Skip it there.
if [ "$NANO" = 0 ]; then
	printf '\nconsole="comconsole"\ncomconsole_port="0x2F8"\ncomconsole_speed="115200"\n' \
		>> "$MNT/boot/loader.conf"
fi

sync
umount "$MNT"
mdconfig -d -u "${DEV#md}"
trap - EXIT

log "wrote $IMG"
cat <<EOF

Write it to USB. Identify the stick first - device names are whatever the host
you are standing at happens to call it, and naming the wrong one destroys that
machine. Check size and partitions with gpart show before naming of=:

  ls /dev/da*; gpart show /dev/da<N>
  dd if=$(basename "$IMG") of=/dev/da<N> bs=1m conv=sync

bs=1m or larger; bs=1 on a stick fails "Invalid argument".

On the 520B, serial console 115200 8N1 (UART1 @ 0x2F8). What to look for, in order:

  1. "ichwd0: <Intel Avoton/Rangeley SoC watchdog timer>"
       -> the dog attached. "ICH WDT present but disabled in BIOS or hardware"
          means clear_noreboot() failed and servicing is impossible.
  2. igb0..igb5 attach in ascending-MAC order (igb2 cc:32 first). If a name
     shifts, /conf/config.xml is pointing at the wrong port - see README,
     "The hardware".
  3. "e6000sw0: <Marvell 88E6176 Switch Address 0>" on mdio0 and mdio1. Empty
     devinfo for mdio/etherswitch means the newbus fix did not take.
  4. The VCE520B-PROBE block printed to serial (--no-probe skips it). Copy the
     whole thing out: pciconf -lv, kldstat, ifconfig -a, devinfo, and whether
     /dev/etherswitch0/1 appeared.
  5. First boot on the serial base - expect live-media behaviour, not a bug:
       mount | grep ' / '        -> read-only, exactly as the image's fstab says.
       mount | grep union        -> boot conf etc home root usr var unioned over
                                    /tmp/.media by rc, because / is ro. Writes under
                                    those paths go to RAM and vanish at reboot.
       df -h /                   -> still the shipped 2.4 G; the serial image does
                                    not ship /.probe.for.growfs, so nothing grew.
                                    Run: sh install-kmods.sh --grow-root
       ls /boot/modules          -> our four .ko are there. The loader reads them off
                                    the disk before rc runs, so the union layer does
                                    not affect them.
EOF
