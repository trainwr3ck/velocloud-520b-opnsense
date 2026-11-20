# OPNsense on the VeloCloud Edge 520B

Making the four Intel I354 (`8086:1f41`) Ethernet functions work under OPNsense 26.7,
delivered as **loadable kernel modules on a stock OPNsense kernel** so the box can keep
taking OPNsense updates without a kernel rebuild.

Status: the port is **built and measured working on real hardware** - all four I354
functions attach, both Marvell 88E6176 switches configure and answer
`etherswitchcfg`, the two 88E1514 WAN PHYs link, the LAN bridge passes gigabit-class
traffic with a byte-exact checksum, `Ierrs 0 / Idrop 0 / Oerrs 0 /
watchdog_timeouts: 0` on all six interfaces, and the SoC watchdog is serviced with
register-level proof. What is new in
this tree is the **delivery method**: the same code, shipped as `.ko`s instead of
patched into a custom kernel.

Read [`docs/STORAGE.md`](docs/STORAGE.md) before touching a drive. It is the shortest
path to a box that silently forgets everything you did.

## What this tree is

| | |
|---|---|
| Target | OPNsense 26.7 (FreeBSD 15.1), VeloCloud Edge 520B |
| Kernel | **stock OPNsense kernel, unmodified** |
| Our code | `if_vce520b.ko`, `vc_mdio.ko`, `vc_pcu_gpio.ko`, `vc_board.ko` in `/boot/modules` |
| Everything else | stock `mdio.ko`, `etherswitch.ko`, `e6000sw.ko`, `ichwd.ko` from `/boot/kernel`, untouched |
| License | BSD-2-Clause. No code from any third-party port of this board is used. |

## The hardware

VeloCloud Edge 520B: Intel Atom C2358 (Rangeley/Avoton SoC), 4 GB, one I350 pair and one
I354 quad-function part, two Marvell 88E6176 LAN switches, two Marvell 88E1514 WAN PHYs,
a PCA9557 I2C GPIO expander, and coreboot as firmware.

**The rear-panel map is [`docs/portmap.svg`](docs/portmap.svg)** - all twelve sockets,
measured by a one-lead walk, with the PCI function, switch and switch port under each
jack. It is the authoritative picture; the table below is a summary of it.

| Jacks | Interface | Hardware |
|---|---|---|
| SFP1 / SFP2 | `igb0` / `igb1` | Intel I350 SFP pair (`8086:1522`), PCI 4:0.0 / 4:0.1 - stock `igb`, no cages fitted |
| LAN5-8 | `igb2` | I354 func 0, PCI 0:20.0 - 88E6176 (etherswitch0) on the I354's internal MDIO |
| LAN1-4 | `igb3` | I354 func 1, PCI 0:20.1 - 88E6176 (etherswitch1) on the I354's internal MDIO |
| WAN0 | `igb4` | I354 func 2, PCI 0:20.2 - 88E1514 PHY on a bit-banged MDIO bus (SoC PCU GPIO 13/14) |
| WAN1 | `igb5` | I354 func 3, PCI 0:20.3 - 88E1514 PHY, same bus |

Switch port map, confirmed by walking leads and independently by the vendor
`LAN_IFLIST` table: `LAN1=B1 LAN2=B0 LAN3=B3 LAN4=B2 LAN5=A2 LAN6=A0 LAN7=A1 LAN8=A3`.

**Interface names are stable and are not to be changed.** Six PCI functions, six
interfaces, `igb0`-`igb5`. Attach order is ascending PCI function, which on this board is
also ascending MAC, so the lowest MAC comes up first and the order is always
`igb2`, `igb3`, `igb4`, `igb5`, `igb0`, `igb1`. The **order** is a property of the board;
the **addresses** belong to the individual unit, so read them off the box in front of you
rather than trusting any list this repo could have carried:

```sh
ifconfig -a | grep -E '^[a-z0-9]+:|ether'   # interface -> MAC, in attach order
pciconf -lvc | grep -iE -A6 'i350|i354'     # PCI function -> MAC
```

That ordering is the observed behaviour on this hardware and this tree preserves it: our
module keeps the newbus driver name `igb`, so the devclass and therefore the
`if_initname()` prefix are unchanged and the names, units and MAC order are whatever any
520B shows. Only the module file name is ours.

The console is a **CP2104 USB-UART** on an internal header, not the front-panel mini-USB
jack - VeloCloud exposes no serial port on this board, so serial access is a tap you have
to make yourself. Find it on the unit with `dmesg | grep -iE 'ucom|uart'`; the 16550 is at
`0x2F8` (UART1) at 115200 8N1.

Check the board revision before wiring anything: a **rev B** board has MDIO and SMBus
swapped relative to rev A, and rev A uses a different WAN MDIO pinout. Do not apply a
rev A recipe to rev B.

## Why modules, and what it costs

A custom kernel works - it is the delivery model this tree replaced - but a box running
one cannot take an OPNsense kernel without a rebuild on a FreeBSD 15.1 host. Modules keep the
kernel stock, so the updater's kernel/base step is skipped and updates are
packages-only. Verify it on the box - `pkg query '%n %v' kernel os` finds nothing, and
`opnsense-update -bk -c` returns rc=1 (`docs/STORAGE.md` section 3 gives the block).

The cost is an **ABI lock**: a `.ko` is only safe against the exact kernel it was built
for. This tree refuses to build an image unless every strong undefined symbol in each of
our modules resolves in the kernel actually inside that image. That gate is offline - see
`scripts/make-stock-usb.sh`, "Module ABI gate".

## How the driver attaches (the method)

Three hooks, all inside the e1000 driver, plus one newbus fix:

1. **Probe override.** `igb_vc_probe(hw)` reads NVM words `0x06`/`0x07` and recognises
   the VeloCloud board by the `VC_ID` / `VC_ID_EDGE5X0` constants in `igb_vc.c`, printing
   what it read on every boot. For the
   WAN functions it reroutes `phy.ops.read_reg/write_reg` to the bit-banged MDIO bus and
   pre-fills `phy.addr`/`phy.id`.
2. **Forced SGMII.** The I354-to-88E6176 link is MAC-to-MAC SGMII: `phy->id == 0` with
   `media_type == internal_serdes`, so `setup_physical_interface` is replaced with a
   forced 1000/full, PCS autoneg disabled.
3. **MDIO bus methods** on the `igb` device so `e6000sw` can reach the switch.
4. **The newbus fix, and it is load-bearing.** `pci` has no `bus_probe` /
   `bus_generic_probe` and no `bus_generic_driver_added` - only
   `bus_driver_added=pci_driver_added`. So `mdio_identify()` is never reached under a
   NIC driver and **the `mdio` child is never created**. `igb_vc_attach_mdio_bus()`,
   called from `em_if_attach_post()` and gated by `igb_vc_is_switch_port()` so the WAN
   functions are untouched, creates and attaches it explicitly. Before that fix
   `devinfo | grep -iE 'mdio|etherswitch|e6000'` was empty and the jack ports read
   `Ctrl=0x007c state=0`; after it they read `state=3`.

Driver naming: our module file is `if_vce520b.ko`, but the newbus **driver name stays
`igb`** - `DRIVER_MODULE()`'s first argument is the module name, and the interface name
comes from `driver_t.name` via `if_initname(device_get_name(dev), device_get_unit(dev))`
(`iflib.c:5581`). So the box keeps showing `igb0`-`igb5` in the same order it always
has, and OPNsense's interface assignment in `/conf/config.xml` carries over untouched.
The module wins the four I354 functions by returning `BUS_PROBE_VENDOR` for `8086:1f41`
only, so stock `igb` keeps the I350 pair. Nothing in `/boot/kernel` is modified or
deleted. If our module fails to load, the I354s fall back to stock `igb` under the same
names, the box still boots and the serial console still works - the failure mode is
driver behaviour, never a renamed or missing interface.

## Pitfalls, all measured on this hardware

- **coreboot leaves PCU GPIO 13/14 in their native function.** Writes to `IO_SEL`/`LVL`
  for a native pin are dropped, so `USE_SEL` must be set first - otherwise MDC stays an
  input and the WAN PHYs never answer after a cold boot.
- **The C2000 `GPIO_LVL` register reads output pins back as 0.** Keep a shadow copy.
- **The vendor source's `relval = 0` for the PHY reset is the level left when the vendor
  driver unloads**, not the run level. The reset is active low.
- **Never pulse PCA9557 GPIO 74.** It resets both 88E1514 PHYs and drops the interface
  carrying ssh.
- **`e6000sw` on FreeBSD is a control-plane driver.** It calls `if_alloc()`/`if_initname()`
  per port but never `if_attach()`/`ether_ifattach()`, and there is no `if_transmit` or
  `if_input`. The switch ports do not appear in `ifconfig` and cannot carry traffic;
  configure the switch with `etherswitchcfg` (`/dev/etherswitch0`) and pass traffic
  through the I354 uplink. Forwarding between jacks happens in silicon, in the per-port
  `PORT_VLAN_MAP` (reg `0x06`) that `e6000sw_init_vlan()` installs. Per-port netifs would
  need a DSA-style driver.
- **`Wrong register address` from `e6000sw` is stock chatter on this silicon**, not a
  fault: it is in pristine, unpatched 26.7 `e6000sw.c` too -
  `grep -n 'Wrong register address' sys/dev/etherswitch/e6000sw/e6000sw.c` finds four
  hits (line numbers move between releases, so run the grep rather than carrying them).
  It appears after both switches report `switch is ready`, and traffic works.

## VLANs

The switch runs in **PORT-based VLAN mode**, and that is not a choice the driver offers
at boot: `e6000sw_attach()` hardcodes `e6000sw_set_vlan_mode(sc, ETHERSWITCH_VLAN_PORT)`
(`e6000sw.c:658`). In that mode `e6000sw_init_vlan()` gives every enabled port FID 0 with
every other enabled port as a member, so a flat L2 bridge across the LAN jacks comes free
- verified through `etherswitch1` with a byte-exact checksum over a large transfer
(`dd` + `md5`), or `iperf3` if a rate is wanted. The I354-to-switch link is SGMII at
1 Gb/s, so expect gigabit-class throughput; do not carry a number away from one run.

Two consequences worth knowing before asking for VLANs:

- `e6000sw_set_port_vlan()` rejects tagged members: `"Tagged ports not supported."` So
  `etherswitchcfg vlangroupN` cannot express a tagged trunk while the chip is in PORT mode.
- 8021Q is reachable - `etherswitchcfg` accepts `dot1q`, which calls
  `e6000sw_setconf()` → `e6000sw_set_vlan_mode(DOT1Q)` → `e6000sw_init_vlan()` again.
  **That is the path where the STU entry matters.** The 88E6176 needs a valid STU entry
  for SID 0 before `VTU_LOAD` stores anything, and stock code does not create one, so a
  DOT1Q switch-over on this chip is expected to silently no-op. If and when tagged
  backhaul on the SGMII link is actually wanted, that STU preload goes back into the
  patch series as a deliberate feature - it is not a bug fix, and it is not needed for
  the flat bridge that works today.

Until then the patch series touches `sys/dev/e1000` only, and `e6000sw.ko` ships stock.

- **Watchdog: the node is `/dev/fido`**, not `/dev/fido`'s Linux counterpart - FreeBSD 15
  names it `_PATH_WATCHDOG` in `sys/sys/watchdog.h`. The `hw.watchdog` sysctls cannot
  answer whether the dog is fed; `tests/wdstate` reads `WDIOC_GETTIMELEFT` directly.
  `sbintime_t` is 32.32 fixed point (`SBT_1S = 1<<32`), not nanoseconds.
- **Do not ask the watchdog for 180 s**: that silently becomes 2^38 ns = 274 s. 120 s is
  the value measured to work.

## Layout

```
config.mk                 build configuration: target release, module lists, anchors
docs/STORAGE.md           how OPNsense uses its flash - read this first
docs/portmap.svg          rear-panel jack map: socket -> PCI function -> switch port
overlay/                  files installed into the image (loader.conf.d, rc.syshook.d)
patches/opnsense-26.7/    the in-tree patch series - sys/dev/e1000 only, two files
scripts/fetch-src.sh      sparse checkout of opnsense/src at the pinned branch
scripts/prepare.sh        apply the patch series to the fetched src tree
scripts/build.sh          build the four .ko files - no kernel is ever built here
scripts/make-stock-usb.sh graft them onto a stock OPNsense image (--with-kmods)
scripts/install-kmods.sh  install them on a running box - the update path
scripts/make-patch.sh     regenerate the patch series after editing the src tree
src/vce520b/              if_vce520b.ko: the e1000 driver + igb_vc.c glue + our Makefile
src/vc_mdio/              bit-banged MDIO bus on SoC PCU GPIO
src/vc_pcu_gpio/          Rangeley/Avoton PCU GPIO register driver
src/vc_board/             board identity + PCA9557 expander
tests/wdstate.c           watchdog proof, read WDIOC_GETTIMELEFT on the box
```

Every `src/<module>/` directory holds its own Makefile and its sources together, and
`scripts/build.sh` builds it in place with the objects sent to `work/obj/<module>`.
Nothing is staged or copied first - a stale staging copy is a bug factory.

There is deliberately no kernel config, no `config(8)` block and no in-tree
`sys/conf/files` registration in this tree. Building a kernel is the delivery
model this repo replaced, and a half-built kernel is how a box ends up unbootable.

## Build

Needs a FreeBSD 15.1 build host with the OPNsense sources; the box itself is not a build
machine. `scripts/fetch-src.sh` does a sparse checkout of `opnsense/src` at the branch
for the pinned release, `scripts/prepare.sh` applies the patch series, `scripts/build.sh`
builds the modules against it.

```sh
sh scripts/fetch-src.sh
sh scripts/prepare.sh
sh scripts/build.sh -j 4
```

Build the modules against the **same src commit the target kernel came from** - `uname -v`
on the box prints it. Then there are two install paths, and both follow the same
discipline as a kernel swap (`docs/STORAGE.md` section 4): back up, record both md5s,
atomic rename on the same filesystem, read back, refresh `linker.hints` with `kldxref`.

**New media** - build the whole image on a FreeBSD host (any one with the image file):

```sh
sh scripts/make-stock-usb.sh --with-kmods     # needs root: mdconfig + mount
sh scripts/make-stock-usb.sh --verify         # re-check an image, changes nothing
```

**Running box** - this is the update path, and it is why the delivery model is modules:
a new driver build goes onto the box that is already configured, no re-flash.

```sh
# <dir> is wherever you put the four .ko, install-kmods.sh and config.mk on the box;
# a batch sftp with no `cd` lands them in root's home.
# This repo ships no key and no host: BatchMode=yes only works once your own key is
# already authorised on the box (add it through the GUI or configctl). Drop BatchMode to
# be prompted instead, or copy over the serial console with no network at all.
sftp -b - -o BatchMode=yes root@<lease from `ifconfig igb4>`
sh <dir>/install-kmods.sh <dir>
sh <dir>/install-kmods.sh --rollback    # undo, then reboot
sh <dir>/install-kmods.sh --grow-root   # if the root slice has not been grown
```

Nothing in this repo changes how OPNsense handles its filesystem. The base image decides
that - the fstab this tree ships is the stock serial one,
`/dev/ufs/OPNsense_Install / ufs ro,noatime 1 1`, root is **read-only**, and stock `rc`
responds by unionfs-mounting `boot conf etc home root usr var` over tmpfs - the box
behaves as live media. No script here remounts, unmounts, re-flags or adds a layer; if
`/` is not writable, it stops and says so rather than writing into a union layer and
reporting success. That is also why the modules have to be grafted into the image
offline: the loader reads `/boot/modules` off the disk before `rc` exists. What the
scripts do add is the discipline that makes a write safe: stage beside the target,
atomic rename on the same filesystem, `sync`, read back and compare md5s, with a trap so
the sync happens even if a script dies partway. See
[`docs/STORAGE.md`](docs/STORAGE.md), sections 1 and 2.

## License note

This tree is BSD-2-Clause and contains **no code from any third-party port of this
board**. The approach - stock kernel, modules in `/boot/modules`, loader-driven load
order, probe priority over the built-in driver - is a method, and methods are not code.
The register sequences and board wiring come from VeloCloud's own GPL Linux sources,
analysed as a wiring document; nothing from them is copied into this tree.
