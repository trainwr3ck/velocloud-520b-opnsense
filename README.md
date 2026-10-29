# OPNsense on the VeloCloud Edge 520B

Makes the four Intel I354 (`8086:1f41`) Ethernet functions on a VeloCloud Edge 520B
work under OPNsense 26.7, delivered as **loadable modules on a stock OPNsense kernel**
so the box keeps taking OPNsense updates with no kernel rebuild.

| | |
|---|---|
| Target | OPNsense 26.7 (FreeBSD 15.1), VeloCloud Edge 520B |
| Kernel | stock OPNsense kernel, unmodified |
| Built here | `if_vce520b.ko`, `vc_mdio.ko`, `vc_pcu_gpio.ko`, `vc_board.ko` -> `/boot/modules` |
| Used as shipped | stock `mdio.ko`, `etherswitch.ko`, `e6000sw.ko`, `ichwd.ko` in `/boot/kernel` |
| Patch series | `sys/dev/e1000` only, two files |
| License | BSD-2-Clause |

## The hardware

Intel Atom C2358 (Rangeley SoC), one Intel I350 SFP pair, one Intel I354 quad-function
part, two Marvell 88E6176 LAN switches, two Marvell 88E1514 WAN PHYs, a PCA9557 I2C
GPIO expander, coreboot firmware. This is a **rev B** board: MDIO and SMBus are swapped
relative to rev A, so a rev A wiring recipe does not apply here.

| Jacks | Interface | Hardware |
|---|---|---|
| SFP1 / SFP2 | `igb0` / `igb1` | Intel I350 SFP pair (`8086:1522`), PCI 4:0.0 / 4:0.1, stock `igb` |
| LAN5-8 | `igb2` | I354 func 0, PCI 0:20.0 - 88E6176 (etherswitch0) on the I354's internal MDIO |
| LAN1-4 | `igb3` | I354 func 1, PCI 0:20.1 - 88E6176 (etherswitch1) on the I354's internal MDIO |
| WAN0 | `igb4` | I354 func 2, PCI 0:20.2 - 88E1514 PHY on a bit-banged MDIO bus (SoC PCU GPIO 13/14) |
| WAN1 | `igb5` | I354 func 3, PCI 0:20.3 - 88E1514 PHY, same bus |

Switch port map: `LAN1=B1 LAN2=B0 LAN3=B3 LAN4=B2 LAN5=A2 LAN6=A0 LAN7=A1 LAN8=A3`.

Interface names are stable: `igb0`-`igb5`, in attach order of ascending PCI function,
which on this board is also ascending MAC - the four I354 functions come up first, then
the I350 pair. Read your own with `ifconfig`.

Our module keeps the newbus driver name `igb`, so only the module *file* is renamed -
the interfaces and the OPNsense interface assignment carry over unchanged.

## Build

Needs a FreeBSD 15.1 build host; the 520B is not a build machine. Fetch the OPNsense
sources, apply the patch series, build the modules.

```sh
sh scripts/fetch-src.sh
sh scripts/prepare.sh
sh scripts/build.sh -j 4        # -> out/*.ko
```

Build against the **same src commit the target kernel came from** - `uname -v` on the box
prints it. A `.ko` is only safe against the exact kernel it was built for.

## New media

```sh
fetch -o out/OPNsense-26.7-nano-amd64.img.bz2 \
  https://pkg.opnsense.org/releases/26.7/OPNsense-26.7-nano-amd64.img.bz2
bunzip2 out/OPNsense-26.7-nano-amd64.img.bz2

sh scripts/make-stock-usb.sh --with-kmods    # needs root: mdconfig + mount
sh scripts/make-stock-usb.sh --verify        # re-check an image, changes nothing
```

Output is `out/vce520b-stock.img`. The base is always the `nano` (embedded) flavour:
root is `rw,noatime`, and it grows the filesystem to the whole device on first boot.
`dd` it to USB (`bs=1m conv=sync`).

Root mounts by the UFS label, so boot order decides which drive boots - and two drives
can carry the same label. If you write the internal disk, pull the stick before
powering on.

## Updating the driver on a running box

This is why the delivery model is modules: a new driver build goes onto the box that is
already configured, no re-flash.

```sh
sftp -b - -o BatchMode=yes root@<box address>   # put the four .ko,
                                               # install-kmods.sh,
                                               # fs-window.sh, config.mk
sh /vc/out/xfer/install-kmods.sh /vc/out/xfer
sh /vc/out/xfer/install-kmods.sh --rollback    # undo, then reboot
sh /vc/out/xfer/install-kmods.sh --grow-root   # only if first boot did not grow root
```

## First boot, what to check

1. `ichwd0: <Intel Avoton/Rangeley SoC watchdog timer>` - the dog attached.
2. `igb2`-`igb5` attach in MAC order; `igb0`/`igb1` come up from stock `igb`.
3. `mdio0: <MDIO> on igb2` and `e6000sw0: <Marvell 88E6176>`. No `e6000sw` in
   `devinfo` means the module did not load or the hints are missing.
4. `kldstat` shows our four plus stock `etherswitch`, `e6000sw`, `mdio`, `ichwd`;
   `/dev/etherswitch0` and `1` exist.
5. `df -h /` reports the whole device, not the shipped 3.0 G.

Then assign interfaces in the GUI or with `configctl interface`. WAN is `igb4`; the LAN
jacks hang off `igb2`/`igb3`.

## Things that will bite you

- **The LAN jacks are not netdevs.** `e6000sw` is a control-plane driver: no ifnet per
  switch port, nothing in `ifconfig`, invisible to the GUI. Traffic egresses through the
  I354 uplink and the switch fans out in hardware. PORT mode already bridges every
  enabled port - check with `etherswitchcfg -d /dev/etherswitch1 config` rather than
  reprogramming it.
- `e6000sw: Wrong register address` on this silicon is stock chatter. Not a fault.
- **Never pulse PCA9557 GPIO 74.** It resets both 88E1514 PHYs and drops the WAN
  interface carrying your session.
- **Watchdog node is `/dev/fido`**, not `/dev/watchdog`. Ask for 120 s, never 180.

## Layout

```
config.mk                 release and module lists
patches/opnsense-26.7/    the in-tree patch series - sys/dev/e1000, two files
scripts/fetch-src.sh      sparse checkout of opnsense/src
scripts/prepare.sh        apply the patch series to the fetched src tree
scripts/build.sh          build the four .ko - no kernel is ever built here
scripts/make-stock-usb.sh graft them onto a stock OPNsense image (--with-kmods)
scripts/install-kmods.sh  install them on a running box - the update path
scripts/fs-window.sh      the one write path to the drive
scripts/common.sh         shared helpers, reads config.mk
src/vce520b/              if_vce520b.ko: e1000 driver + igb_vc.c glue
src/vc_mdio/              bit-banged MDIO bus on SoC PCU GPIO
src/vc_pcu_gpio/          Rangeley/Avoton PCU GPIO register driver
src/vc_board/             board identity + PCA9557 expander
```

Each `src/<module>/` holds its Makefile and sources together and builds in place, with
objects in `work/obj/<module>`. There is no kernel config in this tree.

## License note

BSD-2-Clause, and this tree contains **no code from any third-party port of this
board**. The register sequences and board wiring come from VeloCloud's own GPL Linux
sources, read as a wiring document; nothing from them is copied here.
