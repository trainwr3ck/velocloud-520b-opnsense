# SPDX-License-Identifier: BSD-2-Clause
#
# VeloCloud Edge board identification + PCA9557 I2C GPIO expander reset
# release.  Reads smbios.planar.product/version, publishes
# hw.velocloud.{board_id,board_name} and drops the switches/PHYs out of
# reset over bitbang I2C on the PCU GPIO pins. Built in place - this Makefile
# and vc_board.c are one directory, so no .PATH is needed.

KMOD	= vc_board
SRCS	= vc_board.c
SRCS	+= opt_platform.h

CFLAGS	+= -I${VELOVC}/vc_pcu_gpio

EXPORT_SYMS = vc_board_get_id \
	      vc_board_get_name

# MODULE_DEPEND(vc_board, vc_pcu_gpio, ...) is declared in vc_board.c; the
# loader resolves it at load time, so no build-time dependency is needed.

.include <bsd.kmod.mk>
