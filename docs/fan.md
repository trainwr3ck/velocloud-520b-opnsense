# Fan control (Edge 520 v2, optional)

SPDX-License-Identifier: BSD-2-Clause

Under OPNsense nothing drives the 520 v2 fan controller, so the fan runs at its power-on
default (full speed) all the time. This adds a small temperature-controlled on/off loop.
It is optional: nothing here is in `BOOT_KMODS`, `install-kmods.sh` or the image builder.

Only the Edge 520 **v2** (board `edge520b`) is supported by the on/off profile. The module
also recognises the Edge 540 rev B and keeps a curve profile for it, which has not been
tested under OPNsense.

## How it works

- The fan controller is an EMC2104 at I2C address 0x2f on the board's bit-banged bus
  (SC11 = SDA, SC12 = SCL). `vc_fan.ko` reaches it through `vc_pcu_gpio`'s exported pin
  API, so every pin change stays under `vc_pcu_gpio`'s lock and cache and cannot clobber
  `vc_mdio`'s pins.
- PCA9557 at 0x1c, bit 7, gates the fan supply. With bit 7 as an input (the power-on
  state) the fan runs full. Driven high, the EMC2104 PWM output switches the supply.
- The fan is 3-wire, so it only runs or stops. The EMC2104 runs at 38 Hz and the daemon
  writes duty 0 (off) or 255 (on); every duty below 255 stops this fan.
- The daemon (`velo5x0-fan`) takes the hottest `dev.cpu.N.temperature` (coretemp): ON at
  60 C or above, OFF at 50 C or below, and once ON it stays ON for at least 10 minutes
  (`fan.conf`: `on_c`, `off_c`, `onoff_min_run_s`). Drive temperatures from `smartctl` are
  used when it is installed.
- Bits 0 and 1 of the PCA9557 at 0x1c are never written, and 0x18 is never addressed.

## Safety design

Every failure ends with the fan at full speed:

- A kernel watchdog (30 s) writes duty 255 and releases bit 7 if the daemon stops petting
  it (crash, kill -9, hang).
- Releasing bit 7 puts the fan at full by hardware, whatever the EMC2104 is doing, so every
  failure path releases it as well as writing 255: sensor loss, any daemon exception,
  SIGTERM, a bus fault, a failed spin-up check, `rc.d stop`.
- The module releases bit 7 first thing at load, at `shutdown_pre_sync` (before any crash
  dump), at `shutdown_final`, on panic, and at unload (unload is refused if the release is
  not verified).
- The kernel refuses any duty below 255 while the watchdog is disarmed.
- Nothing is written unless the chip identifies as an EMC2104 at load, and the module
  refuses to load unless `vc_board` has already run.
- The daemon proves it has control (full duty verified) before it enables OFF, and checks
  after every OFF-to-ON transition that the tach shows the fan spinning again.

## Known limit

After a reset that skips an orderly shutdown (a hardware-watchdog reboot, a hang plus the
reset button) while the fan is OFF, **the fan stays off from the reset until the early rc
hook loads `vc_fan`**, which releases bit 7 first. The PCA9557 is not reset by a system
reset and the EMC2104 keeps its duty. If the boot stops before that hook (BIOS, loader
prompt, kernel failure) the fan stays off. A cold power cycle resets both chips and the fan
runs full. An orderly reboot, shutdown or panic that reaches the shutdown handlers is not
affected.

## Build

`scripts/build.sh` builds `vc_fan.ko` with the other modules (`--only vc_fan` for just
this one). `vc_fan` depends only on `vc_pcu_gpio`.

## Install (manual; `install-kmods.sh` does not handle it)

On the box, as root, with `vc_pcu_gpio` and `vc_board` already loaded by the normal install:

```
install -m 0444 vc_fan.ko /boot/modules/vc_fan.ko
install -m 0755 extras/fan/usr/local/sbin/velo5x0-fan /usr/local/sbin/
install -m 0755 extras/fan/usr/local/etc/rc.d/velo5x0_fand /usr/local/etc/rc.d/
install -d /usr/local/etc/rc.syshook.d/early /usr/local/etc/velo5x0-fan
install -m 0755 extras/fan/usr/local/etc/rc.syshook.d/early/06-velo-fan \
    /usr/local/etc/rc.syshook.d/early/
install -m 0644 extras/fan/usr/local/etc/velo5x0-fan/fan.conf /usr/local/etc/velo5x0-fan/
install -m 0644 extras/fan/etc/rc.conf.d/velo5x0_fand /etc/rc.conf.d/
```

The daemon needs Python 3 (`/usr/local/bin/python3`). Load the module once by hand and
check it, then enable the service:

```
/usr/local/sbin/velo5x0-fan load
/usr/local/sbin/velo5x0-fan probe      # hw.vc_fan.identified must be 1
echo 'velo5x0_fand_enable="YES"' > /etc/rc.conf.d/velo5x0_fand
service velo5x0_fand start
```

`vc_fan` must never be listed in `loader.conf` or `loader.conf.d`: the early hook loads it.
To remove: `service velo5x0_fand stop`, `kldunload vc_fan`, delete the files above.

Alerts go to syslog (`daemon.crit`) and `/var/run/velo5x0-fan/ALERT`; state is in
`/var/run/velo5x0-fan/state.json`.

## Tests

```
python3 -m pytest -q extras/fan/tests/
```

73 unit tests of the daemon against a fake sysctl surface, plus static checks of the
module source and the shell scripts. They need no FreeBSD host.
