# SPDX-License-Identifier: BSD-2-Clause
#
# Intel Atom C2000 (Rangeley) PCU GPIO controller driver.
#
# Out-of-tree module: it has no upstream FreeBSD equivalent, so it lives in
# src/vc_pcu_gpio and is built against the target kernel with
# scripts/build.sh.  Exports the three symbols below for vc_board/vc_mdio.
# Built in place - this Makefile and vc_pcu_gpio.c are one directory.

KMOD	= vc_pcu_gpio
SRCS	= vc_pcu_gpio.c
SRCS	+= device_if.h bus_if.h pci_if.h
SRCS	+= opt_platform.h

EXPORT_SYMS = vc_gpio_pin_get \
	      vc_gpio_pin_set \
	      vc_gpio_pin_setflags

.include <bsd.kmod.mk>
