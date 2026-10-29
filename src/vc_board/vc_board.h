/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared header for vc_board — VeloCloud Edge board identification.
 * Used by other VeloCloud kmods (e.g., vc_pcu_gpio, igb_vc) to determine
 * the board type and apply board-specific configuration.
 */

#ifndef _VC_BOARD_H_
#define _VC_BOARD_H_

/*
 * Board identification enum.
 * Matches Linux velocloud.h enum vc_edge_id.
 */
enum vc_board_id {
	VC_BOARD_NONE = 0,
	VC_BOARD_EDGE_NEXCOM,
	VC_BOARD_EDGE500,
	VC_BOARD_EDGE510,
	VC_BOARD_EDGE520,
	VC_BOARD_EDGE540,
	VC_BOARD_EDGE520B,	/* Edge 520, PCB rev B (v2.x) */
	VC_BOARD_EDGE540B,	/* Edge 540, PCB rev B (v2.x) */
	VC_BOARD_EDGE800SM,
	VC_BOARD_EDGE1000IFM,
	VC_BOARD_EDGE1000SM,
	VC_BOARD_EDGE1000QAT,
	VC_BOARD_COUNT,
};

/*
 * Get the detected board ID.
 * Returns VC_BOARD_NONE if vc_board module is not loaded or board unknown.
 */
enum vc_board_id vc_board_get_id(void);

/*
 * Get the board name string (e.g., "edge520b").
 * Returns "unknown" if board not detected.
 */
const char *vc_board_get_name(void);

#endif /* _VC_BOARD_H_ */
