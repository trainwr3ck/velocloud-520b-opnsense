# OPNsense storage: how the box uses its flash, and why that decides where our code goes

It is the single most important thing to get right in this project: the storage model
decides where our code may live, and it is what determines whether an OPNsense update
keeps working.

## Provenance: what is constant, and what is only true on one machine

| | what it is | how to re-check it | travels? |
|---|---|---|---|
| **Hardware** | the 520B: C2358, I350/I354, 88E6176, 88E1514, PCA9557, coreboot, UART1 @ `0x2F8`, rev B | boot a 520B | yes - this is the project |
| **Upstream** | OPNsense 26.7 / FreeBSD 15.1 behaviour: fstab, `rc`, `mount(8)`, the updater | download the same release and look | yes, to anyone on the same release |
| **Local** | a byte offset, an md5, a device name, a size from one run, a hostname | nothing - it is one observation on one machine at one time | **no** |

Nothing **local** is load-bearing here. Where an upstream fact is stated, this file
gives the command that derives it, not the number that was read once. If a command and a
quoted value disagree, the command is right and the value is stale.

## 0. What the base image does, and what we leave alone

The base is the **serial** image, `OPNsense-26.7-serial-amd64.img`
(`scripts/make-stock-usb.sh` picks it). Read it yourself rather than trusting this page:

```sh
fetch https://pkg.opnsense.org/releases/26.7/OPNsense-26.7-serial-amd64.img.bz2
bunzip2 OPNsense-26.7-serial-amd64.img.bz2
DEV=$(mdconfig -f OPNsense-26.7-serial-amd64.img)   # whatever node mdconfig hands back
gpart show "/dev/$DEV"
mount -o ro "/dev/${DEV}p4" /mnt && cat /mnt/etc/fstab
```

Upstream, and reproducible on any copy of that release: GPT, four slices - `efi`,
`freebsd-boot`, `freebsd-swap`, `freebsd-ufs` (2.4 G, labelled `OPNsense_Install`). The
root is the **fourth** slice and it is UFS, not ZFS. The disk node is local - a loopback
attach and a running stick get different names, and the name depends on the host - which
is exactly why the fstab mounts by label.

`/etc/fstab` as shipped:

```
# Device                Mountpoint      FStype  Options         Dump    Pass#
/dev/ufs/OPNsense_Install       /               ufs     ro,noatime      1       1
tmpfs                   /tmp            tmpfs   rw,mode=01777   0       0
```

`ro` is flash-wear protection, not a mistake, and it is not a defect this project works
around. The entry mounts the root **by label**, so whichever drive carries
`OPNsense_Install` becomes root. Do not manage, name or inventory disk numbers - the
boot process resolves the root, and boot order is the system default (USB first, then
internal flash). Consequence: with two drives carrying that label, root can come back
off the wrong one. If you write the internal disk, pull the stick before powering on.

Nothing in this repo remounts, unmounts or re-flags a filesystem, and nothing adds a
layer over one. OPNsense's own `rc` deals with writes on the base it shipped with, and
the mount state is that image's decision. What a `ro` root actually implies for the
whole system is the next section, and it is the single most consequential fact here.

## 1. Read-only root turns the whole system into live media

This is stock behaviour, not something we introduce. From the mounted image, `grep -n
'probe.for\|mount_unionfs\|diskless' /mnt/etc/rc` gives the whole mechanism. `/etc/rc`
runs it in this order:

```sh
grow_partition() { ... gpart recover ${DEV}; gpart resize -i ${IDX} ${DEV}; }
mount -fr / 2> /dev/null            # "tunefs may refuse otherwise"
GROWFS_MARKER=/.probe.for.growfs
... tunefs -n / -t loop over the fstab, honouring "# nosoft" and "# notrim" ...
fsck -C -y / ; mount -a             # retry loop
rm -f ${GROWFS_MARKER}              # "clear growfs marker now that we are read/write"

# see if / is writable (aka. not a live media boot)
if _tmpdir=$(mktemp -d -q /.diskless.XXXXXX); then
        rmdir ${_tmpdir}
else
        # fake a writeable environment in some subdirs
        for i in boot conf etc home root usr var; do
                mkdir -p /tmp/.media/${i}
                mount_unionfs /tmp/.media/${i} /${i}
        done
fi
```

`mount -fr /` is **not** a read-write remount, and reading it as one is the easy
mistake. `mount(8)` on FreeBSD 15.1: `-f` forces revocation of write access when
*downgrading* rw to ro, and forces the R/W mount of an *unclean* filesystem. On a root
that fstab mounted `ro`, it changes nothing. So on this image:

1. `/` is `ro` when `rc` runs.
2. `mktemp -d -q /.diskless.XXXXXX` **fails** on a read-only root.
3. `rc` unionfs-mounts `boot conf etc home root usr var` over tmpfs under `/tmp/.media`.
4. `is_install_media()` - which tests `/.probe.for.readonly` - is now true, and the
   console menu says *"running in live mode from install media"*.
5. **Every write under a unioned path lands in RAM and is gone at reboot.**

Corroborating, from the mounted image - these are upstream and checkable by anyone with
the same release:

```sh
mount | grep union                                  # on a running box: six layers
grep -c mount_unionfs /mnt/etc/rc
grep -rn 'probe.for.readonly' /mnt/usr/local/etc/inc /mnt/usr/local/www 2>/dev/null
```

The hits are `is_install_media()` in `config.inc`, two web status widgets, and the
rescue/install script's `INSTALL="/.probe.for.readonly"`. OPNsense's own comment on that
test: *"Despite unionfs underneath, / is still not writeable, making the following the
perfect test for install media."*

To prove the union to yourself on any 520B booted from this image - two commands, no
trust required:

```sh
touch /usr/local/etc/ephemeral-probe
ls -l /tmp/.media/usr/local/etc/ephemeral-probe     # it is here, not on the drive
reboot
ls /usr/local/etc/ephemeral-probe                   # gone
```

The consequence is worth stating: an on-box `pkg upgrade` on this base writes the whole
upgrade to tmpfs and keeps nothing. On a 4 GB box - the 520B's RAM, a hardware fact - with
a swap slice of 512 K (see the `gpart show` output in section 0) that is an OOM gamble,
not a persistence story. Measure the real numbers on the box you are on rather than
assuming them:

```sh
du -sh /tmp/.media/*   ; df -m /tmp ; swapinfo
```

Two traps follow directly:

- `mount -uw /` alone does **not** make a unioned path writable to the drive. The union
  is a separate mount on top of `/`. To write through, drop the union first
  (`umount /boot` succeeds live, rc=0 - nothing holds it), then write.
- The console menu offers an installer, and the installer **has nothing to install
  from**: the serial image has no `/usr/freebsd-dist`, no `*.txz`, and
  `/usr/share/bsdinstall/` holds only `FreeBSD-base.conf`. "Reinstall it" cannot work
  on this media. That was a symptom of the `ro` flag, not a broken OS.

The union layers are the **expected** state on a `ro` root, not a fault. They do not
affect our delivery: the loader reads `/boot/modules` and `/boot/loader.conf.d` off the
disk at boot, before `rc` exists, so the `.ko` written into the image are what load.
What the union does mean is that a *runtime* write to a unioned path lands in RAM -
which is why `scripts/install-kmods.sh` refuses to install on a `ro` root rather than
quietly writing into RAM and reporting success.

## 2. What this project adds: the slice is grown, and our code is in /boot/modules

The fstab is not touched. The shipped `ro,noatime` flags are the base image's decision,
and `scripts/make-stock-usb.sh` deliberately does not rewrite fstab, console or tmpfs.
What is added is the root slice grown to the whole device - the image ships a 2.4 G
slice, and however much of a stick or internal disk that leaves unused is a property of
the media in the box, not of this project - and our code in `/boot/modules`.

```sh
df -h / ; gpart show $(mount | awk '/ on \/ \(ufs/ {print $1}' | sed 's|/dev/||;s/p[0-9]*$//')
```

A write to the drive has to reach the drive, which on this base means it must not land
in a union layer - see section 1. Offline - the image file mounted on any FreeBSD host -
there is no union at all, so grafting is a normal write. On a running box it takes the
discipline that makes a write safe: stage the new file beside the old one, atomic rename
on the same filesystem, `sync`, then read back and compare md5s. Nothing remounts
anything; if `/` is not writable, the scripts stop rather than work around it.

```sh
cp new /boot/modules/x.ko.new && mv -f x.ko.new x.ko   # atomic, same fs
sync
md5 /boot/modules/x.ko        # read back, compare
df -m /
```

**Growing the slice.** The usual way - a `/.probe.for.growfs` file at the root, which
makes `rc` run `gpart resize` + `growfs` - **does not happen on this image**. The grow is
gated in `rc` by `if [ -f ${GROWFS_MARKER} ]`, and the serial image ships no such file:

```sh
ls -a /mnt | grep probe.for        # nothing on the root of a stock serial image
```

So a stock serial boot never grows anything, and this project does not add the marker
either. The grow is a deliberate one-time step:

```sh
sh install-kmods.sh --grow-root     # gpart recover, gpart resize, growfs -y
```

`--grow-root` is idempotent: a slice that already fills the disk is left alone. It
resolves the root from `mount` output and understands all three node schemes - GPT
(`<disk>p4`), MBR (`<disk>s4`) and BSD disklabel (`<disk>a`) - so it never assumes a
slice name, since both the name and the scheme are the host's and the base image's
choice.

Check on every new image:

```sh
df -h /                      # 2.4 G until --grow-root has been run
mount | grep ' / '           # must say "read-only" - root is ro at rest on this base
```

Two gotchas on the grow itself, neither of them specific to this board:

- A GPT image's **backup table sits at the end of the image**, not of the disk, so after
  `dd` of a short image `gpart show` reports `[CORRUPT]` and `gpart resize` fails with
  "table is corrupt". This is a property of dd'ing any short GPT image, not of this
  board. `gpart recover <disk>` repairs it - `--grow-root` runs it first.
- Then `gpart resize -i <slice>` + `growfs -y`. The grow must not touch the boot slices;
  verify rather than assert, by recording their md5s before and after:

  ```sh
  md5 /dev/${disk}p1 /dev/${disk}p2 > /tmp/boot.md5.before   # efi + freebsd-boot
  # ... grow ...
  md5 -c /tmp/boot.md5.before
  ```

## 3. Where our code may and may not live

OPNsense's own updater is the reason this matters. Every claim here is about 26.7 and
every one of them is checkable on a running box - run these rather than trusting the
prose, because the file contents move between releases:

```sh
pkg query '%n %v' kernel os opnsense          # only `opnsense 26.7` comes back
opnsense-update -bk -c ; echo "rc=$?"         # rc=1; likewise -ck, -cb, -bk -c
opnsense-version -a ; opnsense-version -x     # equal -> FORCE is empty
grep -n 'kernel_to_reboot' \
  /usr/local/opnsense/scripts/firmware/check.sh
```

- There is no `kernel` and no `os` package registered, and **no `kernel` package exists
  in the 26.7 repo at all**. So `pkg upgrade` cannot write `/boot/kernel/kernel`.
- `opnsense-update -bk -c` returning **rc=1**, with `FORCE` empty, means the kernel/base
  branch of `/usr/local/opnsense/scripts/firmware/update.sh` is **skipped**: updates are
  packages-only in practice.
- The web GUI still *lists* `base` and `kernel` as updatable with `needs_reboot: 1`. That
  is a **display heuristic**, not a plan: `firmware/check.sh` compares the
  `opnsense-update` *package* version and, on that match, sets
  `kernel_to_reboot=base_to_reboot` unconditionally without ever reaching the
  `opnsense-update -cfk` probe below it. It never reads the kernel. The `grep` above
  prints the exact lines on the release you are on - do not carry a line number away.

Rules that follow, in order:

1. **Never put anything of ours in `/boot/kernel`.** That directory is what an update
   replaces. Our modules go in `/boot/modules`.
2. **Never write to `/boot` casually.** New media gets our modules grafted in offline
   (`make-stock-usb.sh --with-kmods`, on any FreeBSD host with the image file); a later
   driver build goes onto the running box with `scripts/install-kmods.sh`, which stages,
   renames, syncs and reads back.
   The loader reads `/boot/modules` and `/boot/loader.conf.d` off the disk at boot,
   before `rc` runs, so what is written there is what loads.
3. **`/conf` is the only area OPNsense treats as writable by design** - a separate
   slice holding `config.xml`. Interface assignment, switch/VLAN policy and watchdog
   policy belong there (through `configctl` / the GUI) or in
   `/usr/local/etc/rc.syshook.d/`, not baked into `/boot`.
4. Config changes go through OPNsense's own channels. Hand-editing `/conf/config.xml`
   is **forgotten** by the system: sed-written `<ssh>`/`<authorizedkeys>` elements were
   gone after reboot and the box came up factory-default. Verify by re-reading through
   the GUI/`configctl`, never by grepping the XML.
5. When in doubt about a write, do not guess: `mount | grep ' / '`, write, `sync`, read
   the file back. Do not change mount flags to make a write appear to work, and check
   with `mount | grep union` that the path is not unioned over tmpfs - if it is, the
   write is going to RAM (section 1).

## 4. Kernel and rollback discipline

- The kernel on the image **must be a plain ELF**. Check the magic, not a size - the
  size of a stock kernel changes between releases and is not a fact worth carrying:

  ```sh
  head -c 4 /boot/kernel/kernel | od -An -tx1 | tr -d ' \n'   # must be 7f454c46
  gzip -dc /boot/kernel/kernel 2>/dev/null | head -c 4        # must NOT be an ELF
  ```

  (`tr -d ' \n'` matters: `od -An -tx1` pads each byte with two spaces, so grepping for
  `7f 45 4c 46` never matches.) This loader **cannot inflate a kernel**: a gzipped
  payload named `kernel` reaches the OPNsense menu, prints
  `Failed to load kernel 'kernel'` / `can't load 'kernel'` and drops to `OK`.
- Back up `/boot/kernel/kernel` and record both md5s before replacing it.
- At the loader `OK` prompt, `ls -l /boot/kernel/kernel` identifies which build the
  media holds - cheaper than re-flashing to find out.
- A kernel-swap reboot can revert the **interface assignment** in `/conf/config.xml` to
  factory defaults. The interface **names** do not change - `igb0`-`igb5` come up in the
  same ascending-MAC order on every build - but which one is WAN and which is LAN can.
  Re-read `ifconfig` per interface before concluding anything about the ports.
- **Identify the target disk on the machine you are standing at, every time.** Device
  names are local and are the one thing in this whole project that can destroy a
  computer: `dd` the stick, never the host's own boot disk. `ls /dev/da*`, `gpart show`
  each candidate and check its size and partitions before naming `of=`. Nothing in this
  repo hardcodes a target - `make-stock-usb.sh` only ever writes a file in `out/`, and
  the `dd` is a step you take yourself, after looking.
- `dd bs=1` on a stick fails "Invalid argument"; use 512-byte blocks or larger.
