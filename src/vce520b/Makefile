# SPDX-License-Identifier: BSD-2-Clause
#
# Module Makefile for the VeloCloud e1000 driver, built as if_vce520b.ko.
#
# Derived from ${SRCTOP}/sys/modules/em/Makefile (BSD-2-Clause, see LICENSE)
# with four changes:
#   1. igb_vc.c, which lives in this directory alongside this Makefile, is
#      compiled into the module
#   2. mdio_if.h is generated (if_em.c gains mdio bus methods) and the
#      vc_mdio headers are reachable for the exported bitbang MDIO symbols
#   3. KMOD is if_vce520b, and the stock if_igb.ko symlink is gone
#
# The file name is ours; the driver name registered inside it is still `igb`
# (DRIVER_MODULE(igb, pci, igb_driver, ...) in if_em.c), because iflib builds
# the interface name from driver_t.name + unit (sys/net/iflib.c:5581). Renaming
# the driver would rename igb0..igb5 and break /conf/config.xml. Keeping the
# stock symlink would put our .ko where the loader looks for if_igb.ko, which
# is exactly the shadowing this tree avoids: stock modules stay stock.
#
# Built by scripts/build.sh, which sets SRCTOP to the prepared src tree.

.PATH:	${SRCTOP}/sys/dev/e1000
KMOD    = if_vce520b
SRCS    = device_if.h bus_if.h pci_if.h opt_ddb.h opt_inet.h \
	  opt_inet6.h opt_rss.h ifdi_if.h mdio_if.h
SRCS    += $(CORE_SRC) $(LEGACY_SRC)
SRCS	+= $(COMMON_SHARED) $(LEGACY_SHARED) $(PCIE_SHARED)
CORE_SRC = if_em.c em_txrx.c e1000_osdep.c
CORE_SRC += igb_txrx.c
CORE_SRC += igb_vc.c
COMMON_SHARED = e1000_api.c e1000_base.c e1000_phy.c e1000_nvm.c e1000_mac.c \
		e1000_manage.c e1000_vf.c e1000_mbx.c e1000_i210.c
PCIE_SHARED = e1000_80003es2lan.c e1000_ich8lan.c e1000_82571.c e1000_82575.c
LEGACY_SHARED = e1000_82540.c e1000_82542.c e1000_82541.c e1000_82543.c

CFLAGS += -I${SRCTOP}/sys/dev/e1000
CFLAGS += -I${VELOVC}/vc_mdio

.include <bsd.kmod.mk>
