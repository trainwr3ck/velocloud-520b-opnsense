/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared header for vc_mdio GPIO bitbang MDIO bus.
 * Used by other VeloCloud kmods (e.g., igb_vc) to access WAN PHYs.
 */

#ifndef _VC_MDIO_H_
#define _VC_MDIO_H_

/*
 * Read a PHY register via GPIO bitbang MDIO (Clause 22).
 * Returns register value (16-bit) on success, -1 if bus not available.
 */
int vc_mdio_bus_read(int phy, int reg);

/*
 * Write a PHY register via GPIO bitbang MDIO (Clause 22).
 * Returns 0 on success, -1 if bus not available.
 */
int vc_mdio_bus_write(int phy, int reg, int val);

#endif /* _VC_MDIO_H_ */
