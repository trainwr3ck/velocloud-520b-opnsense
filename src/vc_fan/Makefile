# SPDX-License-Identifier: BSD-2-Clause
#
# EMC2104 fan controller module for the Edge 520 v2 (see docs/fan.md).
#
# Built in place like the other modules: scripts/build.sh passes VELOVC (this
# repo's src/) and sends the objects to $WORK/obj/<module> with MAKEOBJDIR.
# Standalone: make -m $SRC/share/mk SYSDIR=$SRC/sys VELOVC=/path/to/src
# kern.mk builds with -Werror by default.
#
# vc_fan is NOT in BOOT_KMODS and must never be in loader.conf: it is loaded
# by extras/fan/.../06-velo-fan once vc_pcu_gpio and vc_board are loaded.

VELOVC?=	${.CURDIR}/..

KMOD=	vc_fan
SRCS=	vc_fan.c opt_platform.h

CFLAGS+=	-I${VELOVC}/vc_pcu_gpio

.include <bsd.kmod.mk>
