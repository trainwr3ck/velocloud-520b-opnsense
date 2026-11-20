# SPDX-License-Identifier: BSD-2-Clause
#
# ---------------------------------------------------------------------------
# OPNsense VCE520B - build configuration
#
# Sourced by every script in scripts/.  Plain shell assignments so both make
# and /bin/sh can consume it.
# ---------------------------------------------------------------------------

# Which OPNsense release to inject into.  Each profile below pins the
# matching opnsense/src branch, the FreeBSD baseline the VeloCloud patch
# series was extracted for, and the patch directory that applies cleanly.
OPNSENSE_TARGET ?= 26.7

# opnsense/src branch for 26.7 (FreeBSD 15.1 based).
SRC_BRANCH_26_7   ?= stable/26.7
FREEBSD_26_7      ?= 15.1
PATCHSET_26_7     ?= opnsense-26.7

SRC_REPO          ?= https://github.com/opnsense/src.git

# ---------------------------------------------------------------------------
# The module set.  FOUR, not seven - verified against the target image, and
# scripts/make-stock-usb.sh --verify asserts it there rather than trusting this line.
#
# patches/opnsense-26.7/velocloud-vce520b.patch touches exactly two files:
#   sys/dev/e1000/e1000_82575.c   probe override, forced SGMII, LED config
#   sys/dev/e1000/if_em.c         mdio bus methods, mdio child attach
#
# Everything else our code depends on ships STOCK, unmodified:
#   mdio        not patched.  The DRIVER_MODULE(mdio, igb, ...) declaration is
#               in if_em.c, not in mdio.c.  Stock mdio.ko is correct.
#   etherswitch not patched.  Stock etherswitch.ko is correct.
#   e6000sw     not patched IN THIS TREE.  patches/opnsense-26.7/ contains only
#               the two e1000 hunks, so stock e6000sw.ko is what ships and what
#               the image check asserts.
#
#   What is true about e6000sw and is NOT in this patch set: the 88E6176 VTU
#   only admits a VID on a port, and the separate Switch Table Unit decides
#   what that port does with the frame.  Programming the VTU alone - all
#   upstream ever does - leaves a VLAN that reads back configured and carries
#   nothing; hand-loading the STU entry by register made a dot1q VLAN pass
#   traffic.  An earlier series carried that as an STU preload and it never
#   ran, because it sat inside `if (sc->vlan_mode == ETHERSWITCH_VLAN_DOT1Q)`
#   while e6000sw_attach() hardcodes ETHERSWITCH_VLAN_PORT (e6000sw.c:658).
#   If it is ever landed, it goes into sys/dev/etherswitch/e6000sw/e6000sw.c,
#   into make-patch.sh PATHS, into KMODS below and into BOOT_KMODS - and
#   scripts/make-stock-usb.sh must then stop asserting a stock e6000sw.ko.
#   It is a feature (tagged backhaul), not a bug fix; the flat PORT-mode
#   bridge this tree ships does not need it.  See README.md, "VLANs".
#
# So we build and install only these, into /boot/modules:
KMODS = vce520b vc_pcu_gpio vc_board vc_mdio

# `vce520b` is the MODULE name (if_vce520b.ko). The newbus DRIVER name inside it
# stays `igb`, so the interfaces remain igb0..igb5 in the order the board has
# always shown them - ascending PCI function, which here is also ascending MAC,
# i.e. igb2, igb3, igb4, igb5, igb0, igb1. The ORDER is a board property; the
# ADDRESSES are per unit, so read them on the box (`ifconfig -a`) instead of
# carrying a list. Interface naming is not ours to change:
# /conf/config.xml and the GUI assignment carry over unchanged. See README.md,
# "The hardware".

# Stock modules we depend on and deliberately do NOT replace. Listed so a build
# can assert they are present in the target image instead of assuming it.
# e6000sw is here because this tree does not patch it - see the note above.
STOCK_KMODS = mdio etherswitch e6000sw ichwd

# Load order: if_vce520b.ko has `U vc_mdio_bus_read/write`, which vc_mdio.ko
# provides on top of vc_pcu_gpio.ko. kldload pulls dependencies itself; the
# explicit order in loader.conf documents it.
BOOT_KMODS = vc_pcu_gpio vc_board vc_mdio vce520b

# ---------------------------------------------------------------------------
# Anchor commits verified when the patch series were extracted.  A build
# against a different commit still works if `git apply` succeeds, but these
# are the exact trees the patches were generated from.
# ---------------------------------------------------------------------------
ANCHOR_opnsense_26_7 ?= 12334a596709d195acb73f0f069da82c570a3d84

# uname -v on a box running that tree prints:
#   stable/26.7-n283674-12334a596709
# Build our modules against that commit, not a later one - the ABI gate in
# scripts/make-stock-usb.sh compares every strong undefined symbol against the
# kernel actually inside the image.

# Blob shas of the unpatched files the 26.7 patch series was diffed against:
#   sys/dev/e1000/e1000_82575.c  47b8006314f8a3c03e95ab60562adc4dea11f6e2
#   sys/dev/e1000/if_em.c        23b7e3ff116b76601ab28d9841181fd6ba1463a2
