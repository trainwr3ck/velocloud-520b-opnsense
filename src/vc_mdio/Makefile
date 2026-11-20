# SPDX-License-Identifier: BSD-2-Clause
#
# GPIO bitbang MDIO bus (IEEE 802.3 Clause 22) for the VeloCloud WAN PHYs.
#
# Drives SC13 (data) / SC14 (clock) on the Atom C2000 PCU GPIO controller to
# reach the two Marvell 88E1514 PHYs (bus addresses 0 and 1).  igb_vc.c calls
# the two exported symbols directly, so this module has to be loaded before
# if_vce520b (scripts/make-stock-usb.sh writes the kld_load list in that order).
#
# Built in place: this Makefile and vc_mdio.c are one directory, so no .PATH is
# needed - bmake searches .CURDIR. scripts/build.sh sends the objects to
# $WORK/obj/<module> with MAKEOBJDIR.

KMOD	= vc_mdio
SRCS	= vc_mdio.c
SRCS	+= opt_platform.h

CFLAGS	+= -I${VELOVC}/vc_pcu_gpio

EXPORT_SYMS = vc_mdio_bus_read \
	      vc_mdio_bus_write

.include <bsd.kmod.mk>
