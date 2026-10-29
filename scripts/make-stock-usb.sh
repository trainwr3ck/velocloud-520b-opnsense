#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# shellcheck shell=sh
# VeloCloud Edge 520B image: stock OPNsense kernel + our code as loadable modules.
#
#   scripts/make-stock-usb.sh                     # -> out/vce520b-stock.img
#   scripts/make-stock-usb.sh --image X-nano.img  # graft onto another nano image
#   scripts/make-stock-usb.sh --mode disable      # disarm the dog instead of petting
#   scripts/make-stock-usb.sh --with-kmods        # install out/*.ko into /boot/modules
#   scripts/make-stock-usb.sh --verify            # inspect an existing image, change nothing
#
# --with-kmods is the delivery switch: the kernel stays stock OPNsense and
# everything of ours ships as loadable modules in /boot/modules. It refuses to
# run unless the ABI check below passes.
#
# mdio.ko, etherswitch.ko, e6000sw.ko and ichwd.ko are stock and not rebuilt.
# The base must be the OPNsense `nano` (embedded) flavour.
#
# What it changes on the target root (kernel and stock modules are left alone):
#   boot/loader.conf.d/vce520b-console.conf  console on UART1 @ 0x2F8
#   boot/loader.conf.d/vce520b-wdt.conf      ichwd_load="YES"
#   boot/loader.conf.d/vce520b-kmods.conf    module load order (--with-kmods)
#   boot/loader.conf.d/vce520b-switch.conf   e6000sw device hints
#   etc/rc.conf.d/watchdogd                  pet the dog
#   usr/local/etc/vce520b-wdt.conf           feeder policy
#   usr/local/etc/rc.syshook.d/early/00-watchdog
#
# REQUIRES ROOT: mdconfig and mount are privileged.

set -eu
. "$(dirname "$0")/common.sh"

STOCK_IMG=""
WDT_MODE=service
VERIFY=0
WITH_KMODS=0
MNT="$WORK/mnt"

while [ $# -gt 0 ]; do
	case "$1" in
	--image) STOCK_IMG=$2; shift ;;
	--mode) WDT_MODE=$2; shift ;;
	--with-kmods) WITH_KMODS=1 ;;
	--verify) VERIFY=1 ;;
	-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	# Flag-only options do not shift themselves; without this the loop spins.
	shift
done

case "$WDT_MODE" in
service | disable) ;;
*) die "--mode must be 'service' or 'disable', got: $WDT_MODE" ;;
esac

# mdconfig/mount fail with a confusing error deep in the script; check up front.
[ "$(id -u)" = 0 ] || die "run as root on the build host (mdconfig + mount)"

resolve_target
mkdir -p "$TOP/out"

# --------------------------------------------------------------------------
# Module ABI gate (--with-kmods): every strong undefined symbol in our .ko must
# resolve in the kernel inside the image, or in another of our modules.
# --------------------------------------------------------------------------
OUR_KMODS="$BOOT_KMODS"

# config.mk module name -> .ko basename. Only vce520b differs: if_vce520b.ko.
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
	# are ifuncs in the 26.7 kernel.
	kern_syms=$(mktemp)
	nm -g "$kernel" 2>/dev/null | awk '$2 ~ /[TtDdBbRrVvWwi]/ { print $3 }' |
		sort -u >"$kern_syms"
	# Symbols our other modules export. Here `nm -g` is WRONG: bsd.kmod.mk runs
	# kmod_syms.awk + objcopy, which marks everything not in EXPORT_SYMS local
	# (`-L`), so a loadable module legitimately has almost no GLOBAL symbols. The
	# kernel linker reads the whole .symtab of a loaded module, so compare against
	# `nm` (all bindings) and only exclude the undefined ones.
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
# --verify: attach the image already built and prove every file landed.
# --------------------------------------------------------------------------
if [ "$VERIFY" = 1 ]; then
	IMG="$TOP/out/vce520b-stock.img"
	[ -f "$IMG" ] || die "no $IMG - build it first"
	log "verifying $IMG ($(stat -f %z "$IMG") bytes)"
	DEV=$(mdconfig -f "$IMG")
	trap 'umount "$MNT" 2>/dev/null || true; mdconfig -d -u "${DEV#md}" 2>/dev/null || true' EXIT
	UFS=""
	for s in a b c d e f g h; do
		node="/dev/${DEV}${s}"
		[ -e "$node" ] || continue
		if fstyp "$node" 2>/dev/null | grep -qi ufs; then UFS="$node"; break; fi
	done
	[ -n "$UFS" ] || die "no UFS partition in $IMG"
	mkdir -p "$MNT"
	mount -o ro "$UFS" "$MNT"

	fail=0
	for f in boot/loader.conf.d/vce520b-console.conf \
		boot/loader.conf.d/vce520b-wdt.conf \
		etc/rc.conf.d/watchdogd \
		usr/local/etc/vce520b-wdt.conf \
		usr/local/etc/rc.syshook.d/early/00-watchdog; do
		if [ -f "$MNT/$f" ]; then
			printf '  ok   %s  %s\n' "$f" "$(stat -f '%Sp %z' "$MNT/$f")"
		else
			printf '  MISS %s\n' "$f"
			fail=1
		fi
	done

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
		if grep -q '^hint.e6000sw.1.at=' \
			"$MNT/boot/loader.conf.d/vce520b-switch.conf" 2>/dev/null; then
			printf '  ok   switch hints (vce520b-switch.conf)\n'
		else
			printf '  MISS boot/loader.conf.d/vce520b-switch.conf - the switches will not attach\n'
			fail=1
		fi
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

# The nano (embedded) flavour: BSD disklabel not GPT, root `rw,noatime` by
# design, and /.probe.for.growfs makes rc grow the filesystem on first boot.
# Storage and console defaults are left as shipped.
if [ -z "$STOCK_IMG" ]; then
	STOCK_IMG=$(ls "$TOP"/out/OPNsense-*-nano-amd64.img 2>/dev/null | head -1 || true)
fi
[ -n "$STOCK_IMG" ] && [ -f "$STOCK_IMG" ] || die \
"no stock image in out/. Fetch it:
  fetch -o out/OPNsense-${OPNSENSE_TARGET}-nano-amd64.img.bz2 \\
    https://pkg.opnsense.org/releases/${OPNSENSE_TARGET}/OPNsense-${OPNSENSE_TARGET}-nano-amd64.img.bz2
  bunzip2 out/OPNsense-${OPNSENSE_TARGET}-nano-amd64.img.bz2"
case "$STOCK_IMG" in
*-nano-*) ;;
*) die "base must be a nano (embedded) image, got: $STOCK_IMG" ;;
esac

IMG="$TOP/out/vce520b-stock.img"
log "base: $STOCK_IMG"
log "copying -> $IMG"
cp "$STOCK_IMG" "$IMG"

# --------------------------------------------------------------------------
# Attach and find the UFS slice. BSD disklabel, so the node is named by letter
# (/dev/md0a) - probe the letters, there is no md0p1 or md0s1 on this base.
# --------------------------------------------------------------------------
log "attaching $IMG"
DEV=$(mdconfig -f "$IMG")
trap 'umount "$MNT" 2>/dev/null || true; mdconfig -d -u "${DEV#md}" 2>/dev/null || true' EXIT

UFS=""
for s in a b c d e f g h; do
	node="/dev/${DEV}${s}"
	[ -e "$node" ] || continue
	if fstyp "$node" 2>/dev/null | grep -qi ufs; then
		UFS="$node"
		break
	fi
done
if [ -z "$UFS" ]; then
	log "what the kernel actually sees on $DEV:"
	ls "/dev/${DEV}"* 2>&1 | sed 's/^/    /'
	gpart show "/dev/$DEV" 2>&1 | sed 's/^/    /'
	die "no UFS slice found in $IMG"
fi
log "UFS slice: $UFS"

# The nano image's UFS comes out of the .img with the clean flag unset, so an rw
# mount is refused until it is checked. fsck the copy in the image, never the
# shipped base - nothing is mounted there yet.
if fsck -y "$UFS" >/dev/null 2>&1; then
	log "fsck clean"
else
	log "fsck reported repairs (expected on the nano base)"
fi

mkdir -p "$MNT"
mount -o rw "$UFS" "$MNT"

# Prove the kernel really is stock.
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
# Console: the 520B's is UART1 @ 0x2F8, not the com1 address the defaults assume.
# Which console devices are enabled is left as the base image ships them.
# --------------------------------------------------------------------------
mkdir -p "$MNT/boot/loader.conf.d" "$MNT/etc/rc.conf.d" \
	"$MNT/usr/local/etc" "$MNT/usr/local/etc/rc.syshook.d/early"

# --------------------------------------------------------------------------
# --with-kmods: our drivers in /boot/modules, never /boot/kernel - that is the
# directory `opnsense-update` replaces. Nothing stock is deleted or shadowed.
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
# VeloCloud Edge 520B: console is UART1 @ 0x2F8, not the com1 address.
comconsole_port="0x2F8"
comconsole_speed="115200"
# Kernel-side console selection is separate from the loader's: uart0 is
# disabled, uart1 carries the console.
hint.uart.0.flags="0x0"
hint.uart.1.flags="0x10"
EOF
# Which console devices are enabled stays as the base image ships them - adding a
# console= line here would override that.

cat > "$MNT/boot/loader.conf.d/vce520b-wdt.conf" <<'EOF'
# Stock FreeBSD driver for the Avoton/Rangeley SoC watchdog (sys/dev/ichwd).
# ichwd_attach() calls ichwd_tmr_disable() during attach, which halts the
# timer the firmware armed at reset. That is the whole point of loading it.
ichwd_load="YES"
EOF

# --------------------------------------------------------------------------
# Watchdog petting.
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

# rc.syshook.d/early runs before any daemon starts, so this covers the gap
# between kernel attach and watchdogd. Hand off as soon as watchdogd is up - it
# has the pretimeout console warning, which this loop cannot do.
cat > "$MNT/usr/local/etc/rc.syshook.d/early/00-watchdog" <<'EOF'
#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD 15 names the watchdog node /dev/fido (_PATH_WATCHDOG,
# sys/sys/watchdog.h:37) - there is no /dev/watchdog on this base.
# `watchdog -d` is DEBUGGING, not disable; `watchdog -t 0` is the disable.
# Keep PERIOD a power of two, or just under one: the kernel rounds the request
# up to the next power of two in nanoseconds, so 120 -> 137 s and 180 -> 274 s.
DEV=/dev/fido
CONF=/usr/local/etc/vce520b-wdt.conf
PATH=/sbin:/bin:/usr/sbin:/usr/bin
MODE=service
PERIOD=120
NAP=30
if [ -f "$CONF" ]; then
	. "$CONF"
fi
MODE="${VCE520B_WDT_MODE:-$MODE}"
PERIOD="${VCE520B_WDT_PERIOD:-$PERIOD}"
NAP="${VCE520B_WDT_NAP:-$NAP}"

say() {
	printf 'vce520b-wdt: %s\n' "$1"
	[ -x /usr/bin/logger ] && /usr/bin/logger -t vce520b-wdt -- "$1"
	return 0
}

(
	if [ ! -c "$DEV" ]; then
		say "MISSING $DEV - cannot service or disarm; expect a reboot"
		exit 1
	fi
	if [ "$MODE" = disable ]; then
		if watchdog -t 0 2>/dev/null; then
			say "DISARMED via $DEV (box is NOT protected)"
		else
			say "FAILED to disarm via $DEV - firmware timer may still run"
		fi
		exit 0
	fi
	say "servicing $DEV: period ${PERIOD}s, pat every ${NAP}s"
	stuck=0
	while true; do
		if pgrep -x watchdogd >/dev/null 2>&1; then
			say "watchdogd is up - handing off"
			exit 0
		fi
		if watchdog -t "$PERIOD" 2>/dev/null; then
			stuck=0
		elif [ "$stuck" -eq 0 ]; then
			stuck=1
			say "watchdog -t $PERIOD failed (ichwd refused: NO_REBOOT set?)"
		fi
		sleep "$NAP"
	done
) >/dev/console 2>&1 &
EOF
chmod 0755 "$MNT/usr/local/etc/rc.syshook.d/early/00-watchdog"

# --------------------------------------------------------------------------
# Module load order + switch hints, only for the module-delivery image.
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
		# we say so - and then the mdio bus is created under igb with no driver on
		# it and the LAN jacks stay dead. Loading them before if_vce520b means the
		# driver is registered when igb attaches and creates the bus.
		printf 'etherswitch_load="YES"\n'
		printf 'e6000sw_load="YES"\n'
	} >"$MNT/boot/loader.conf.d/vce520b-kmods.conf"
	# The e6000sw device hints. A stock kernel has no compiled-in hint table,
	# so loader.conf.d is the only place they can come from.
	cat > "$MNT/boot/loader.conf.d/vce520b-switch.conf" <<'EOF'
hint.e6000sw.0.is6190=0
hint.e6000sw.0.is6190x=0
hint.e6000sw.0.at="mdio0"
hint.e6000sw.0.addr="0"
hint.e6000sw.0.port4cpu="1"
hint.e6000sw.0.port4speed="1000"
hint.e6000sw.0.port5disabled=1
hint.e6000sw.0.port6disabled=1
hint.e6000sw.1.is6190=0
hint.e6000sw.1.is6190x=0
hint.e6000sw.1.at="mdio1"
hint.e6000sw.1.addr="0"
hint.e6000sw.1.port4cpu="1"
hint.e6000sw.1.port4speed="1000"
hint.e6000sw.1.port5disabled=1
hint.e6000sw.1.port6disabled=1
EOF
	# Storage: nothing. The shipped fstab is the embedded design (rw,noatime,
	# notrim) and /.probe.for.growfs is what takes the whole device on first boot.
	# Leave both alone.
	log "fstab left as shipped: $(tail -1 "$MNT/etc/fstab")"
	[ -e "$MNT/.probe.for.growfs" ] &&
		log "growfs marker present - root grows to the whole device on first boot"
fi

sync
umount "$MNT"
mdconfig -d -u "${DEV#md}"
trap - EXIT

log "wrote $IMG"
cat <<EOF

Write it to USB:
  dd if=$(basename "$IMG") of=/dev/da0 bs=1m conv=sync

On the 520B, console at 115200 8N1 (UART1 @ 0x2F8). What to look for:

  ichwd0: <Intel Avoton/Rangeley SoC watchdog timer>   -> the dog attached
  igb2..igb5 attach, then igb0/igb1                    -> MAC order, see README
  e6000sw0/1: <Marvell 88E6176 ...> on mdio0/mdio1     -> switches attached
  kldstat: if_vce520b vc_mdio vc_pcu_gpio vc_board + stock etherswitch,
           e6000sw, mdio, ichwd; /dev/etherswitch0 and /dev/etherswitch1
  df -h /                                              -> the whole device
EOF
