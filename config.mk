# SPDX-License-Identifier: BSD-2-Clause
#
# ---------------------------------------------------------------------------
# OPNsense VCE520B - build configuration
#
# Sourced by every script in scripts/.  Plain shell assignments so both make
# and /bin/sh can consume it.
# ---------------------------------------------------------------------------

# Which OPNsense release to inject into.
OPNSENSE_TARGET ?= 26.7

# opnsense/src branch for 26.7 (FreeBSD 15.1 based).
SRC_BRANCH_26_7   ?= stable/26.7
FREEBSD_26_7      ?= 15.1
PATCHSET_26_7     ?= opnsense-26.7

SRC_REPO          ?= https://github.com/opnsense/src.git

# ---------------------------------------------------------------------------
# The module set.  Our code goes into /boot/modules as loadable modules; the
# patch series touches two files under sys/dev/e1000.  mdio, etherswitch,
# e6000sw and ichwd are used as OPNsense ships them.
# ---------------------------------------------------------------------------
KMODS = vce520b vc_pcu_gpio vc_board vc_mdio

# `vce520b` is the module name (if_vce520b.ko); the newbus driver name inside
# it is `igb`, so the interfaces stay igb0..igb5.

# Stock modules we depend on and do not replace.
STOCK_KMODS = mdio etherswitch e6000sw ichwd

# Load order.
BOOT_KMODS = vc_pcu_gpio vc_board vc_mdio vce520b