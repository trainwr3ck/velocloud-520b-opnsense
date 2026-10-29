/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 VeloCloud/VMware
 *
 * VeloCloud I354 customization for FreeBSD if_em/if_igb.
 *
 * Edge 520B I354 port mapping:
 *   func0: 88E6176 LAN switch A (SGMII, I354 internal MDIO)
 *   func1: 88E6176 LAN switch B (SGMII, I354 internal MDIO)
 *   func2: 88E1514 WAN0 PHY (SGMII, GPIO bitbang MDIO, PHY addr 0)
 *   func3: 88E1514 WAN1 PHY (SGMII, GPIO bitbang MDIO, PHY addr 1)
 *
 * Based on Linux igb_vc.c from VeloCloud patches-3.14.
 *
 * The patch series is anchored to opnsense/src 12334a596709d195acb73f0f069da82c570a3d84.
 */

#include "e1000_api.h"
#include "e1000_defines.h"
#include "e1000_regs.h"
#include "if_em.h"

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/systm.h>
#include <net/iflib.h>

/* Import GPIO bitbang MDIO from vc_mdio module */
extern int vc_mdio_bus_read(int phy, int reg);
extern int vc_mdio_bus_write(int phy, int reg, int val);

/* VeloCloud EEPROM identification words at offset 0x06/0x07 */
#define	VC_ID		0x5663	/* "Vc" */
#define	VC_ID_EDGE5X0	0x3558	/* "5X" — edge520/edge540 */

/* How many leading NVM words to scan when the marker is not where we expect */
#define	VC_NVM_SCAN	0x40

/* I354 internal MDIO timeout: 100 * 10us = 1ms */
#define	VC_INTMDIO_TIMEOUT	100

/*
 * Diagnostic verbosity.
 *
 * The MDIO bus scan and register dumps in section 6 are expensive (64 MDIO
 * transactions) and only useful while bringing the board up.  Enable with
 * hint.igb_vc.debug="1" in kernel hints / loader.conf, or with -v on the
 * kernel boot line (bootverbose).  Off otherwise, so a production box does
 * not spam the console and reprogram MDICNFG dozens of times per attach.
 */
static int igb_vc_debug = 0;
TUNABLE_INT("hw.igb_vc.debug", &igb_vc_debug);

/*
 * Force the VeloCloud path on an I354 even when the EEPROM customer words
 * at 0x06 do not read back VC_ID.
 *
 * The marker offset was inferred from the vendor Linux tree. It has since been
 * confirmed on this box - the EEPROM reads back [0x06]=0x5663 and [0x07]=0x3558
 * on both switch functions - so this is no longer the reason it exists. It is
 * kept because it is a recovery lever: if the patch ever silently no-ops, the
 * box comes up as stock FreeBSD with two working ports, which looks like an
 * inert patch rather than a wrong one.
 *
 * Settable from the loader prompt, which is what makes it worth having:
 * at the "Type '?' for help; ..." prompt, enter
 *
 *   set hw.igb_vc.force 1
 *   boot
 *
 * and the next boot exercises the switch path without reflashing.  A
 * TUNABLE_INT is read by the kernel before any driver attaches, so the
 * loader prompt is enough; no rebuild, no re-image.
 */
static int igb_vc_force = 0;
TUNABLE_INT("hw.igb_vc.force", &igb_vc_force);

static int
igb_vc_verbose(void)
{
	return (igb_vc_debug || bootverbose);
}

/*
 * ===== Section 1: GPIO bitbang MDIO (WAN ports, func2/func3) =====
 */

/*
 * MDIO read via GPIO bitbang (vc_mdio module).
 * Conforms to e1000_phy_operations.read_reg(hw, offset, *data).
 */
static s32
igb_vc_mdiobb_read(struct e1000_hw *hw, u32 offset, u16 *data)
{
	int ret;

	ret = vc_mdio_bus_read(hw->phy.addr, offset & 0x1f);
	if (ret < 0)
		return -E1000_ERR_PHY;
	*data = (u16)ret;
	return E1000_SUCCESS;
}

/*
 * MDIO write via GPIO bitbang (vc_mdio module).
 * Conforms to e1000_phy_operations.write_reg(hw, offset, data).
 */
static s32
igb_vc_mdiobb_write(struct e1000_hw *hw, u32 offset, u16 data)
{
	int ret;

	ret = vc_mdio_bus_write(hw->phy.addr, offset & 0x1f, data);
	if (ret < 0)
		return -E1000_ERR_PHY;
	return E1000_SUCCESS;
}

/*
 * ===== Section 2: I354 internal MDIO (LAN switch ports, func0/func1) =====
 *
 * These functions use the I354's E1000_MDIC/E1000_MDICNFG registers
 * to access the 88E6176 switch via the I354's built-in MDIO bus.
 *
 * For single-chip mode (sw_addr=0), the 'phy' parameter from
 * e6000sw is the 5-bit device address within the switch:
 *   0x00-0x06: per-port registers
 *   0x1b: global 1
 *   0x1c: global 2
 */

/*
 * Wait for I354 internal MDIO completion.
 * Returns the 16-bit data on success, or < 0 on error.
 */
static s32
igb_vc_intmdio_wait(struct e1000_hw *hw)
{
	int i;
	u32 mdic;

	for (i = 0; i < VC_INTMDIO_TIMEOUT; i++) {
		DELAY(10);
		mdic = E1000_READ_REG(hw, E1000_MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	if (!(mdic & E1000_MDIC_READY)) {
		printf("igb_vc: internal MDIO read did not complete\n");
		return -E1000_ERR_PHY;
	}
	if (mdic & E1000_MDIC_ERROR) {
		printf("igb_vc: internal MDIO error\n");
		return -E1000_ERR_PHY;
	}
	return (s32)(mdic & 0xffff);
}

/*
 * MDICNFG save / restore.
 *
 * Every switch access has to point MDICNFG at the external MDIO pins and at
 * the switch's 5-bit device address, which means temporarily clobbering the
 * destination bit and the PHY-address field.  Leaving them clobbered is not
 * safe:
 *
 *   - e1000_phy.c never writes MDICNFG.  e1000_read_phy_reg_mdic() and
 *     e1000_write_phy_reg_mdic() put the PHY address in the MDIC register
 *     and assume MDICNFG.Destination is whatever the NVM left, so a stock PHY
 *     access that races with ours would be routed to the external bus and
 *     read the switch instead of the PHY.
 *   - e1000_get_phy_id_82575() reads the PHY address back out of MDICNFG
 *     (e1000_82575.c, the e1000_sgmii_uses_mdio_82575() branch) for 82580,
 *     i350, i354, i210 and i211.  If our last switch address happens to be
 *     sitting there, phy->addr is wrong for the rest of the interface's life.
 *   - e1000_reset_mdicnfg_82580(), the driver's own repair path, bails out
 *     unless mac.type == e1000_82580, so nothing else puts it back.
 *
 * So treat MDICNFG as borrowed: read it, program it, and restore the original
 * value once the transaction completes.  Same save/restore shape the in-tree
 * e1000_i210.c MDIO workaround uses.
 */
static __inline void
igb_vc_mdicnfg_set(struct e1000_hw *hw, u32 saved, u32 addr)
{
	u32 mdicnfg;

	mdicnfg = saved;
	mdicnfg &= ~E1000_MDICNFG_PHY_MASK;
	mdicnfg |= E1000_MDICNFG_EXT_MDIO;
	mdicnfg |= (addr << E1000_MDICNFG_PHY_SHIFT);
	E1000_WRITE_REG(hw, E1000_MDICNFG, mdicnfg);
	E1000_WRITE_FLUSH(hw);
}

static __inline void
igb_vc_mdicnfg_restore(struct e1000_hw *hw, u32 saved)
{

	E1000_WRITE_REG(hw, E1000_MDICNFG, saved);
	E1000_WRITE_FLUSH(hw);
}

/*
 * Read a register on the 88E6176 via I354 MDIC register.
 * Conforms to e1000_phy_operations.read_reg(hw, offset, *data).
 *
 * For single-chip addressing: 'offset' encodes both device address
 * and register:  offset[9:5] = device addr, offset[4:0] = register
 *
 * The 88E6176 switch management registers are accessed via external
 * MDC/MDIO pins, so we must force EXT_MDIO=1 (NVM defaults to 0,
 * which routes MDIC to the internal SGMII PCS).
 */
static s32
igb_vc_intmdio_read(struct e1000_hw *hw, u32 offset, u16 *data)
{
	u32 addr, saved, mdic;
	s32 ret;

	/* Single-chip: device address from offset[9:5] */
	addr = (offset >> 5) & 0x1f;

	/* Acquire PHY semaphore for MDIC bus arbitration */
	if (hw->phy.ops.acquire) {
		ret = hw->phy.ops.acquire(hw);
		if (ret) {
			if (igb_vc_verbose())
				printf("igb_vc: MDIO read acquire failed (%d)\n",
				    ret);
			return (ret);
		}
	}

	/*
	 * Program MDICNFG: force EXT_MDIO=1 for external switch access and
	 * point the PHY address field at the target device.  COM_MDIO is left
	 * exactly as the NVM set it.  Restored below.
	 */
	saved = E1000_READ_REG(hw, E1000_MDICNFG);
	igb_vc_mdicnfg_set(hw, saved, addr);

	/* Issue read: register from offset[4:0] */
	mdic = E1000_MDIC_OP_READ |
	    ((offset & 0x1f) << E1000_MDIC_REG_SHIFT);
	E1000_WRITE_REG(hw, E1000_MDIC, mdic);

	ret = igb_vc_intmdio_wait(hw);

	igb_vc_mdicnfg_restore(hw, saved);

	/* Release PHY semaphore */
	if (hw->phy.ops.release)
		hw->phy.ops.release(hw);

	if (ret < 0)
		return (ret);

	*data = (u16)ret;
	return (E1000_SUCCESS);
}

/*
 * Write a register on the 88E6176 via I354 MDIC register.
 * Conforms to e1000_phy_operations.write_reg(hw, offset, data).
 */
static s32
igb_vc_intmdio_write(struct e1000_hw *hw, u32 offset, u16 data)
{
	u32 addr, saved, mdic;
	s32 ret;

	/* Single-chip: device address from offset[9:5] */
	addr = (offset >> 5) & 0x1f;

	/* Acquire PHY semaphore for MDIC bus arbitration */
	if (hw->phy.ops.acquire) {
		ret = hw->phy.ops.acquire(hw);
		if (ret) {
			if (igb_vc_verbose())
				printf("igb_vc: MDIO write acquire failed (%d)\n",
				    ret);
			return (ret);
		}
	}

	/*
	 * Program MDICNFG: force EXT_MDIO=1 for external switch access.
	 * COM_MDIO is left exactly as the NVM set it.  Restored below - see
	 * the comment on igb_vc_mdicnfg_set().
	 */
	saved = E1000_READ_REG(hw, E1000_MDICNFG);
	igb_vc_mdicnfg_set(hw, saved, addr);

	/* Issue write: register from offset[4:0], data in lower 16 bits */
	mdic = E1000_MDIC_OP_WRITE |
	    ((offset & 0x1f) << E1000_MDIC_REG_SHIFT) |
	    (u32)data;
	E1000_WRITE_REG(hw, E1000_MDIC, mdic);

	ret = igb_vc_intmdio_wait(hw);

	igb_vc_mdicnfg_restore(hw, saved);

	/* Release PHY semaphore */
	if (hw->phy.ops.release)
		hw->phy.ops.release(hw);

	if (ret < 0)
		return (ret);

	return (E1000_SUCCESS);
}

/*
 * ===== Section 3: MDIO bus methods for if_em device =====
 *
 * These are called from the FreeBSD mdio bus framework when
 * e6000sw performs MDIO_READREG(parent, phy, reg).
 *
 * The device_t 'dev' here is the igb PCI device.
 * With iflib, device_get_softc(dev) returns if_ctx_t,
 * from which we get e1000_softc via iflib_get_softc().
 *
 * For non-VeloCloud boards or WAN ports, these return ENXIO.
 */

/* Flag: check if PHY ops point to our internal MDIO functions */
static int
igb_vc_is_switch_port(struct e1000_hw *hw)
{
	return (hw->phy.ops.read_reg == igb_vc_intmdio_read);
}

/*
 * Custom bus_add_child: create the `mdio` child only on switch ports
 * (func0/func1), so e6000sw has a bus to attach to.
 *
 * Who calls this, and why it is not enough on its own:
 * DRIVER_MODULE(mdio, igb, mdio_driver, 0, 0) in sys/dev/e1000/if_em.c puts
 * mdio in the `igb` devclass, which is what lets a child named `mdio` under an
 * igb function find a driver.  It does not create that child.
 *
 * mdio_identify() is only ever reached through DEVICE_IDENTIFY, and the only
 * two things that call DEVICE_IDENTIFY against an igb device are:
 *   bus_identify_children() (subr_bus.c:3439), reached only from
 *     bus_generic_probe() (:3425) - igb never gets there, pci.c has no
 *     bus_probe method and iflib_device_attach() does not call it; and
 *   bus_generic_driver_added() (:3890) via devclass_driver_added() (:709),
 *     which notifies only devices where device_is_attached() is true, and only
 *     for a driver registered into the devclass *after* that device attached.
 * igb attaches during config(8) time and mdio is registered into the `igb`
 * devclass by the same kernel image, so neither route fires.
 *
 * Measured on the installed kernel (md5 2ac7d0a184c981fe8e2eaad5b39d66b0):
 * nm shows mdio_driver, e6000sw_driver and etherswitch_driver all compiled in,
 * igb_vc's own probe/diag output is on the console for func0 and func1, yet
 * `devinfo | grep -iE 'mdio|etherswitch|e6000'` is empty and /dev/etherswitch*
 * does not exist.  Drivers present, child never created.
 *
 * So the child is created explicitly, by igb_vc_attach_mdio_bus() below, from
 * em_if_attach_post().  hint.e6000sw.N.at="mdioN" then lands on it through
 * mdio_attach()'s bus_enumerate_hinted_children().
 *
 * IMPORTANT: the gate is on the child *name*, not on every child.  An earlier
 * version returned NULL for anything that was not a switch port, which would
 * have silently refused unrelated children on the WAN functions — the only
 * two ports that work on stock today.  Anything that is not our mdio bus
 * falls through to the normal bus_add_child behaviour.
 */
device_t igb_vc_bus_add_child(device_t dev, u_int order, const char *name,
    int unit);
device_t
igb_vc_bus_add_child(device_t dev, u_int order, const char *name, int unit)
{
	if_ctx_t ctx;
	struct e1000_softc *sc;
	int is_switch;

	/* Only the mdio bus child is subject to the switch-port gate. */
	if (name == NULL || strcmp(name, "mdio") != 0)
		return (device_add_child_ordered(dev, order, name, unit));

	is_switch = 0;
	ctx = device_get_softc(dev);
	if (ctx != NULL) {
		sc = iflib_get_softc(ctx);
		if (sc != NULL)
			is_switch = igb_vc_is_switch_port(&sc->hw);
	}

	/*
	 * No mdio child on WAN ports: their 88E1514 PHYs are driven in-driver
	 * over GPIO bit-bang MDIO by vc_mdio, not through an mdio(4) bus.
	 */
	if (!is_switch)
		return (NULL);

	return (device_add_child_ordered(dev, order, name, unit));
}

/*
 * Create and attach the mdio(4) child on a switch function.
 *
 * Called from em_if_attach_post(): iflib has attached the igb device, the
 * VeloCloud probe has replaced phy.ops with the internal MDIO accessors (so
 * igb_vc_is_switch_port() means something here), and igb_vc_verify_switch()
 * has just read the 88E6176 through the MDIC successfully.
 *
 * device_probe_and_attach() on the child runs mdio_probe()/mdio_attach();
 * mdio_attach() calls bus_enumerate_hinted_children(), which is what turns
 * hint.e6000sw.N.at="mdioN" into the e6000sw device, and bus_attach_children()
 * attaches it.  Nothing else in the tree ever does this for igb.
 */
void igb_vc_attach_mdio_bus(device_t dev);
void
igb_vc_attach_mdio_bus(device_t dev)
{
	if_ctx_t ctx;
	struct e1000_softc *sc;
	device_t child;

	ctx = device_get_softc(dev);
	if (ctx == NULL)
		return;
	sc = iflib_get_softc(ctx);
	if (sc == NULL || !igb_vc_is_switch_port(&sc->hw))
		return;

	child = device_find_child(dev, "mdio", DEVICE_UNIT_ANY);
	if (child == NULL)
		child = igb_vc_bus_add_child(dev, 0, "mdio", DEVICE_UNIT_ANY);
	if (child == NULL) {
		device_printf(dev, "igb_vc: no mdio bus child\n");
		return;
	}
	if (device_is_attached(child))
		return;
	if (device_probe_and_attach(child) != 0)
		device_printf(dev, "igb_vc: mdio bus attach failed\n");
}

/*
 * MDIO bus readreg — called by e6000sw via mdio framework.
 * Signature: int mdio_readreg(device_t dev, int phy, int reg)
 */
int igb_vc_mdio_readreg(device_t dev, int phy, int reg);
int
igb_vc_mdio_readreg(device_t dev, int phy, int reg)
{
	if_ctx_t ctx;
	struct e1000_softc *sc;
	struct e1000_hw *hw;
	u16 data;
	s32 ret;

	ctx = device_get_softc(dev);
	if (ctx == NULL)
		return (ENXIO);
	sc = iflib_get_softc(ctx);
	if (sc == NULL)
		return (ENXIO);
	hw = &sc->hw;

	if (!igb_vc_is_switch_port(hw))
		return (ENXIO);

	/*
	 * e6000sw calls MDIO_READREG(dev, phy, reg) where:
	 *   phy = 5-bit device address in switch (port, global1, global2)
	 *   reg = 5-bit register within that device
	 *
	 * Our intmdio functions encode this as: offset = (phy << 5) | reg
	 */
	ret = igb_vc_intmdio_read(hw, ((u32)phy << 5) | ((u32)reg & 0x1f),
	    &data);
	if (ret != E1000_SUCCESS)
		return (ENXIO);

	return ((int)data);
}

/*
 * MDIO bus writereg — called by e6000sw via mdio framework.
 * Signature: int mdio_writereg(device_t dev, int phy, int reg, int val)
 */
int igb_vc_mdio_writereg(device_t dev, int phy, int reg, int val);
int
igb_vc_mdio_writereg(device_t dev, int phy, int reg, int val)
{
	if_ctx_t ctx;
	struct e1000_softc *sc;
	struct e1000_hw *hw;
	s32 ret;

	ctx = device_get_softc(dev);
	if (ctx == NULL)
		return (ENXIO);
	sc = iflib_get_softc(ctx);
	if (sc == NULL)
		return (ENXIO);
	hw = &sc->hw;

	if (!igb_vc_is_switch_port(hw))
		return (ENXIO);

	ret = igb_vc_intmdio_write(hw, ((u32)phy << 5) | ((u32)reg & 0x1f),
	    (u16)val);
	if (ret != E1000_SUCCESS)
		return (ENXIO);

	return (0);
}

/*
 * ===== Section 4: VeloCloud board probe =====
 */

/*
 * Dump the head of the NVM and look for the VeloCloud marker anywhere in it.
 *
 * Why this exists: the marker offset 0x06 is copied from the vendor Linux
 * tree, and the vendor tree we have is for the **edge500**, not the 520B.
 * If the 520B's customer words sit at a different offset, igb_vc_probe()
 * returns 0 for all four I354 functions, the whole patch no-ops, and the box
 * fails exactly the way it already does - with one number on the console and
 * no hint which of the two possible causes it was.
 *
 * There are exactly two candidate failure sites for "Setup of Shared code
 * failed, error -2" on an I354, and they are distinguishable from the
 * register state we already print:
 *
 *   e1000_82575.c:307  default: of switch (phy->id) in
 *                      e1000_init_phy_params_82575().  Reached when
 *                      e1000_get_phy_id_82575() succeeds but reads back an
 *                      id that is not in the table (0, or 0xffff).
 *   e1000_82575.c:730  "A valid PHY type couldn't be found" in
 *                      e1000_get_phy_id_82575(), i.e. the I2C/SGMII address
 *                      sweep 1..7 found no M88_VENDOR.  Only reachable when
 *                      sgmii_active && !sgmii_uses_mdio, which the printed
 *                      MDICNFG.ext_mdio bit tells us.
 *
 * So: dump the NVM, and one boot tells us whether the marker exists at all
 * and where it actually lives.  Cheap (one bulk read) and only on the
 * failure path.
 */
static void
igb_vc_nvm_dump(struct e1000_hw *hw, u8 func)
{
	u16 words[VC_NVM_SCAN];
	s32 ret;
	int i, found;

	printf("igb_vc: func%u NVM: type=%d word_size=%u address_bits=%u "
	    "page_size=%u opcode_bits=%u EECD=0x%08x\n",
	    func, hw->nvm.type, hw->nvm.word_size, hw->nvm.address_bits,
	    hw->nvm.page_size, hw->nvm.opcode_bits,
	    E1000_READ_REG(hw, E1000_EECD));

	memset(words, 0, sizeof(words));
	ret = hw->nvm.ops.read(hw, 0, VC_NVM_SCAN, words);
	printf("igb_vc: func%u NVM bulk read @0 len=%u ret=%d\n",
	    func, VC_NVM_SCAN, ret);
	if (ret)
		return;

	for (i = 0; i < VC_NVM_SCAN; i += 8)
		printf("igb_vc: func%u NVM[%04x-%04x]: %04x %04x %04x %04x "
		    "%04x %04x %04x %04x\n", func, i, i + 7,
		    words[i], words[i + 1], words[i + 2], words[i + 3],
		    words[i + 4], words[i + 5], words[i + 6], words[i + 7]);

	found = -1;
	for (i = 0; i < VC_NVM_SCAN; i++)
		if (words[i] == VC_ID) {
			found = i;
			break;
		}
	if (found >= 0)
		printf("igb_vc: func%u marker 0x%04x found at NVM offset "
		    "0x%02x (next word 0x%04x) - set the read offset here\n",
		    func, VC_ID, found,
		    found + 1 < VC_NVM_SCAN ? words[found + 1] : 0);
	else
		printf("igb_vc: func%u marker 0x%04x NOT present in NVM "
		    "0x00-0x%02x - this board does not use the edge500 "
		    "customer words\n", func, VC_ID, VC_NVM_SCAN - 1);
}

/*
 * Probe for VeloCloud Edge board.
 *
 * Called from e1000_init_phy_params_82575() early, before PHY ID
 * detection.
 *
 * For WAN ports (func2/3):
 *   Overrides phy.ops to GPIO bitbang MDIO, reads PHY ID.
 *
 * For LAN switch ports (func0/1):
 *   Overrides phy.ops to I354 internal MDIO.
 *   Sets media_type to internal_serdes to bypass copper PHY paths.
 *   Reads switch product ID via internal MDIO register.
 *
 * Returns:
 *   1  — VeloCloud port detected, phy ops/addr/id prepared
 *   0  — not VeloCloud, caller does standard init
 */
s32 igb_vc_probe(struct e1000_hw *hw);
s32
igb_vc_probe(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	u16 eeprom[2];
	u32 status, ctrl_ext, mdicnfg;
	u8 func;
	u16 id1, id2;
	s32 nvm_ret;
	int is_vc;

	/* Only applies to I354 */
	if (hw->mac.type != e1000_i354)
		return 0;

	/* Get bus function number */
	status = E1000_READ_REG(hw, E1000_STATUS);
	func = (u8)((status & E1000_STATUS_FUNC_MASK) >>
		     E1000_STATUS_FUNC_SHIFT);

	/*
	 * Bring-up telemetry, printed unconditionally.
	 *
	 * On the real 520B all four I354 functions died with
	 * "Setup of Shared code failed, error -2" == -E1000_ERR_PHY, raised
	 * inside e1000_init_phy_params_82575() -> e1000_get_phy_id_82575().
	 * igb_vc_probe() is the first hook on that path, so this is the only
	 * place the board's actual register state can be captured before the
	 * attach aborts. Without it the failure is unobservable: the stock
	 * driver prints one number and gives up.
	 *
	 * CTRL_EXT bits 23:22 (E1000_CTRL_EXT_LINK_MODE_MASK 0x00C00000) are
	 * the link mode: 00 GMII, 01 1000BASE-KX, 10 SGMII, 11 PCIe SERDES.
	 * MDICNFG decides whether MDIC reaches the internal SGMII PCS or the
	 * external MDC/MDIO pins - that single bit is what the whole switch
	 * design hangs on.
	 */
	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	mdicnfg = E1000_READ_REG(hw, E1000_MDICNFG);
	printf("igb_vc: func%u I354 probe: CTRL_EXT=0x%08x link_mode=%u "
	    "MDICNFG=0x%08x (ext_mdio=%d com_mdio=%d phy_addr=%u)\n",
	    func, ctrl_ext,
	    (ctrl_ext & E1000_CTRL_EXT_LINK_MODE_MASK) >>
		E1000_CTRL_EXT_LINK_MODE_OFFSET,
	    mdicnfg,
	    !!(mdicnfg & E1000_MDICNFG_EXT_MDIO),
	    !!(mdicnfg & E1000_MDICNFG_COM_MDIO),
	    (mdicnfg & E1000_MDICNFG_PHY_MASK) >> E1000_MDICNFG_PHY_SHIFT);

	/*
	 * Read EEPROM customer words at offset 0x06/0x07.
	 *
	 * The first word is the VeloCloud marker; the second names the board.
	 * Only the marker is required.  The vendor edge500 Linux tree uses a
	 * different second word (VC_ID_EDGE500 0x6535) than the edge5x0 one
	 * (0x3558), and the 520B value has never been read on real hardware.
	 * Hard-failing on a mismatch would disable the entire patch silently,
	 * which is the worst failure mode available: the box would come up
	 * looking like stock FreeBSD with two working ports and nothing else.
	 * Log it and carry on - e1000_i354 has already narrowed the match.
	 */
	eeprom[0] = eeprom[1] = 0;
	nvm_ret = hw->nvm.ops.read(hw, 0x06, 2, eeprom);
	is_vc = (eeprom[0] == VC_ID);
	printf("igb_vc: func%u NVM read @0x06 ret=%d "
	    "EEPROM[0x06]=0x%04x EEPROM[0x07]=0x%04x -> %s\n",
	    func, nvm_ret, eeprom[0], eeprom[1],
	    is_vc ? "VeloCloud" : "NOT VeloCloud");
	if (!is_vc && igb_vc_force) {
		printf("igb_vc: func%u hw.igb_vc.force=1 - proceeding on the "
		    "e1000_i354 match alone\n", func);
		is_vc = 1;
	}
	if (!is_vc) {
		/*
		 * Printing this unconditionally matters: if the NVM read
		 * itself failed, or the customer words are somewhere else on
		 * this board, the patch silently no-ops and the box looks
		 * like stock FreeBSD. That is the failure mode we already
		 * suspect, so say so out loud.
		 */
		printf("igb_vc: func%u falling back to stock PHY init - "
		    "expect -E1000_ERR_PHY if this port has no real PHY\n",
		    func);
		igb_vc_nvm_dump(hw, func);
		return 0;
	}

	/* Board id is informational, not a gate. */
	if (eeprom[1] != VC_ID_EDGE5X0)
		printf("igb_vc: func%u VeloCloud marker present, board id "
		    "0x%04x (expected 0x%04x for Edge 5x0) - continuing\n",
		    func, eeprom[1], VC_ID_EDGE5X0);

	if (func < 2) {
		/*
		 * LAN switch port (func0/func1).
		 *
		 * EEPROM already confirmed this is a VeloCloud Edge 5x0.
		 * Trust the board identification — don't try to read the
		 * switch product ID via MDIC here.  At this point in init
		 * (e1000_init_phy_params_82575), I354 hardware hasn't been
		 * through e1000_reset_hw yet, so MDIC external access fails.
		 *
		 * Switch verification happens later when e6000sw attaches
		 * through the mdio bus (after full hardware init).
		 */
		phy->addr = 0;		/* single-chip addressing */
		phy->ops.read_reg = igb_vc_intmdio_read;
		phy->ops.write_reg = igb_vc_intmdio_write;
		phy->media_type = e1000_media_type_internal_serdes;
		hw->phy.media_type = e1000_media_type_internal_serdes;

		/*
		 * Set phy->id = 0 so the vc_phy_ready path in
		 * e1000_82575.c detects this as a switch port and
		 * skips M88E1512 PHY init entirely.
		 */
		phy->id = 0;
		phy->revision = 0;

		printf("igb_vc: VeloCloud Edge 5x0, func%u, "
		    "LAN switch port (88E6176 assumed)\n", func);

		return 1;
	}

	/* WAN port: override MDIO to GPIO bitbang */
	phy->addr = func - 2;	/* func2→PHY0, func3→PHY1 */
	phy->ops.read_reg = igb_vc_mdiobb_read;
	phy->ops.write_reg = igb_vc_mdiobb_write;

	/* Read PHY ID via bitbang */
	if (phy->ops.read_reg(hw, PHY_ID1, &id1))
		return 0;
	if (phy->ops.read_reg(hw, PHY_ID2, &id2))
		return 0;

	/* Apply same masking as e1000_get_phy_id() in e1000_phy.c */
	phy->id = ((u32)id1 << 16) | (u32)(id2 & PHY_REVISION_MASK);
	phy->revision = (u32)(id2 & ~PHY_REVISION_MASK);

	printf("igb_vc: VeloCloud Edge 5x0, func%u, "
	    "PHY addr %u, ID 0x%08x (rev %u)\n",
	    func, phy->addr, phy->id, phy->revision);

	/*
	 * Both 88E1512 (rev 0) and 88E1514 (rev 1) mask to
	 * M88E1512_E_PHY_ID (0x01410DD0) after PHY_REVISION_MASK.
	 */
	if (phy->id != M88E1512_E_PHY_ID) {
		printf("igb_vc: unexpected PHY ID 0x%08x "
		    "(expected 0x%08x)\n",
		    phy->id, M88E1512_E_PHY_ID);
		return 0;
	}

	return 1;	/* tell caller: phy ops/addr/id are ready */
}

/*
 * ===== Section 4B: Forced SGMII link for switch ports =====
 *
 * The I354-to-88E6176 connection is MAC-to-MAC SGMII.
 * Standard serdes autoneg (e1000_setup_serdes_link_82575) does not
 * work — the switch CPU port expects a forced link.
 *
 * Ported from Linux igb_vc_setup_sgmii_link_forced().
 * Used as mac->ops.setup_physical_interface for switch ports.
 */
s32 igb_vc_setup_sgmii_forced(struct e1000_hw *hw);
s32
igb_vc_setup_sgmii_forced(struct e1000_hw *hw)
{
	u32 cfg, ctrl, lctl;
	u16 data;
	s32 ret;

	/*
	 * Step 0: SerDes initialization normally done by
	 * e1000_setup_serdes_link_82575() which we bypass.
	 *
	 * a) Disable SerDes loopback — after hardware reset, the
	 *    loopback state persists until explicitly cleared.
	 *    Without this, the I354 may receive its own TX data
	 *    (or nothing at all) instead of external RX data.
	 *
	 * b) Power on SerDes/SFP cage by clearing SDP3_DATA.
	 *    This enables the SerDes transceiver for both TX and RX.
	 */
	{
		u32 sctl_before = E1000_READ_REG(hw, E1000_SCTL);
		printf("igb_vc: SCTL before=0x%08x (loopback %s)\n",
		    sctl_before,
		    (sctl_before & E1000_SCTL_DISABLE_SERDES_LOOPBACK)
		    ? "disabled" : "ENABLED/unknown");
	}
	E1000_WRITE_REG(hw, E1000_SCTL,
	    E1000_SCTL_DISABLE_SERDES_LOOPBACK);

	ctrl = E1000_READ_REG(hw, E1000_CTRL_EXT);
	ctrl &= ~E1000_CTRL_EXT_SDP3_DATA;
	E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl);
	E1000_WRITE_FLUSH(hw);

	/*
	 * Step 0c: Ensure SerDes data path is enabled.
	 * After e1000_reset_hw_82580(), the CONNSW register may have
	 * SERDESD (bit 9) set, which disables the SerDes data path.
	 * Since we bypass e1000_setup_serdes_link_82575() (which handles
	 * auto-sense switching), SERDESD can remain set from hardware
	 * reset, effectively muting the SerDes receiver.
	 */
	{
		u32 connsw = E1000_READ_REG(hw, E1000_CONNSW);
		printf("igb_vc: CONNSW before=0x%08x (SERDESD=%d)\n",
		    connsw, !!(connsw & E1000_CONNSW_SERDESD));
		if (connsw & E1000_CONNSW_SERDESD) {
			connsw &= ~E1000_CONNSW_SERDESD;
			E1000_WRITE_REG(hw, E1000_CONNSW, connsw);
			E1000_WRITE_FLUSH(hw);
			printf("igb_vc: cleared CONNSW.SERDESD\n");
		}
	}

	/* Step 1: Enable PCS */
	cfg = E1000_READ_REG(hw, E1000_PCS_CFG0);
	cfg |= E1000_PCS_CFG_PCS_EN;
	E1000_WRITE_REG(hw, E1000_PCS_CFG0, cfg);

	/* Step 2: SLU + force 1000/FD */
	ctrl = E1000_READ_REG(hw, E1000_CTRL);
	ctrl |= (E1000_CTRL_SLU
	    | E1000_CTRL_SPD_1000
	    | E1000_CTRL_FD
	    | E1000_CTRL_FRCSPD
	    | E1000_CTRL_FRCDPX);
	E1000_WRITE_REG(hw, E1000_CTRL, ctrl);

	/* Step 3: Force SGMII link — disable autoneg */
	lctl = E1000_READ_REG(hw, E1000_PCS_LCTL);
	lctl &= ~(E1000_PCS_LCTL_AN_ENABLE
	    | E1000_PCS_LCTL_AN_RESTART
	    | E1000_PCS_LCTL_AN_TIMEOUT);
	lctl |= (E1000_PCS_LCTL_FLV_LINK_UP
	    | E1000_PCS_LCTL_FSV_1000
	    | E1000_PCS_LCTL_FDV_FULL
	    | E1000_PCS_LCTL_FSD
	    | E1000_PCS_LCTL_FORCE_FCTRL
	    | E1000_PCS_LCTL_FORCE_LINK);
	E1000_WRITE_REG(hw, E1000_PCS_LCTL, lctl);

	/*
	 * Step 4: Reset the switch's internal SerDes PHY.
	 *
	 * Linux: igb_vc_m88sw_sgmii_reset() via Global 2 SMI indirect PHY access.
	 *
	 * 88E6176 architecture:
	 *   - SerDes PHY for SGMII is accessed indirectly via Global 2 registers
	 *   - Global 2 = device address 0x1C
	 *   - SMI PHY Data = register 0x18
	 *   - SMI PHY Command = register 0x19
	 *   - SerDes device = 0xF, page = 1
	 *   - PHY_CONTROL (BMCR) = register 0
	 *
	 * SMI CMD format: [15]=Busy [13:12]=Op(01=Wr,11=Rd) [9:5]=Dev [4:0]=Reg
	 *   MODE bit (bit 12) is set for 22-bit clause mode.
	 */
	{
#define GL2_DEV		0x1C	/* Global 2 device address */
#define GL2_SMI_CMD	0x18	/* SMI PHY Command register */
#define GL2_SMI_DATA	0x19	/* SMI PHY Data register */
#define SMI_BUSY	0x8000
#define SMI_MODE_22	0x1000	/* Clause 22 mode (bit 12) */
#define SMI_OP_WRITE	0x0400	/* bits [11:10]=01 */
#define SMI_OP_READ	0x0800	/* bits [11:10]=10 */
#define SERDES_DEV	0xF	/* SerDes PHY device in SMI */
#define SERDES_PAGE	1	/* Page 1 = SerDes registers */
#define PHY_PAGE_REG	22	/* Page select register */
#define BMCR_RESET	0x8000
#define BMCR_AN_EN	0x1000
#define BMCR_POWER_DN	0x0800
#define BMCR_FULL_DPX	0x0100

		u16 smi_cmd, bmcr;
		int i;

		/* Note: igb_vc_intmdio_read/write handle acquire/release */

		/* 4a: Set MDICNFG to point to Global 2 (dev 0x1C) */
		/* Use intmdio helpers which set EXT_MDIO for us */

		/* 4b: Wait for SMI not busy */
		for (i = 0; i < 100; i++) {
			igb_vc_intmdio_read(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
			    &smi_cmd);
			if (!(smi_cmd & SMI_BUSY))
				break;
			DELAY(10);
		}
		if (i >= 100) {
			printf("igb_vc: SMI busy timeout\n");
			goto serdes_done;
		}

		/* 4c: Set SerDes PHY page to 1 (write page reg 22 = 1) */
		data = SERDES_PAGE;
		igb_vc_intmdio_write(hw, (GL2_DEV << 5) | GL2_SMI_DATA,
		    data);
		smi_cmd = SMI_BUSY | SMI_OP_WRITE | SMI_MODE_22
		    | (SERDES_DEV << 5) | PHY_PAGE_REG;
		igb_vc_intmdio_write(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
		    smi_cmd);

		/* Poll for completion */
		for (i = 0; i < 100; i++) {
			igb_vc_intmdio_read(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
			    &smi_cmd);
			if (!(smi_cmd & SMI_BUSY))
				break;
			DELAY(10);
		}

		/* 4d: Read BMCR (reg 0) from SerDes PHY */
		smi_cmd = SMI_BUSY | SMI_OP_READ | SMI_MODE_22
		    | (SERDES_DEV << 5) | 0;
		igb_vc_intmdio_write(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
		    smi_cmd);

		for (i = 0; i < 100; i++) {
			igb_vc_intmdio_read(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
			    &smi_cmd);
			if (!(smi_cmd & SMI_BUSY))
				break;
			DELAY(10);
		}

		igb_vc_intmdio_read(hw, (GL2_DEV << 5) | GL2_SMI_DATA,
		    &bmcr);
		printf("igb_vc: SerDes PHY BMCR=0x%04x\n", bmcr);

		/* 4e: Set RESET | FULL_DUPLEX, clear AN_EN | POWER_DOWN */
		bmcr |= (BMCR_RESET | BMCR_FULL_DPX);
		bmcr &= ~(BMCR_AN_EN | BMCR_POWER_DN);

		igb_vc_intmdio_write(hw, (GL2_DEV << 5) | GL2_SMI_DATA,
		    bmcr);
		smi_cmd = SMI_BUSY | SMI_OP_WRITE | SMI_MODE_22
		    | (SERDES_DEV << 5) | 0;
		igb_vc_intmdio_write(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
		    smi_cmd);

		for (i = 0; i < 100; i++) {
			igb_vc_intmdio_read(hw, (GL2_DEV << 5) | GL2_SMI_CMD,
			    &smi_cmd);
			if (!(smi_cmd & SMI_BUSY))
				break;
			DELAY(10);
		}

		DELAY(100);  /* Allow reset to complete */
		printf("igb_vc: SerDes PHY reset done (wrote BMCR=0x%04x)\n",
		    bmcr);

serdes_done:
		; /* label needs a statement */
	}

	/*
	 * Step 5: Clear PHYDetect on switch CPU port (port 4).
	 * The 88E6176 sets PHYDET (bit 12) in the Port Status register
	 * when it detects a SerDes PHY.  This must be cleared for the
	 * SGMII data path to work.
	 *
	 * CPU port = dev addr 0x14, Port Status = reg 0.
	 * M88_SW_PORT_STATUS_PHYDET = (1 << 12) = 0x1000
	 */
	ret = igb_vc_intmdio_read(hw, (0x14 << 5) | 0, &data);
	if (ret == E1000_SUCCESS) {
		printf("igb_vc: switch port4 status BEFORE=0x%04x "
		    "(PHYDET=%d LINK=%d)\n",
		    data, !!(data & 0x1000), !!(data & 0x0800));
		if (data & 0x1000) {
			data &= ~0x1000;	/* Clear PHYDET */
			igb_vc_intmdio_write(hw, (0x14 << 5) | 0, data);
			/* Read back to confirm */
			igb_vc_intmdio_read(hw, (0x14 << 5) | 0, &data);
			printf("igb_vc: switch port4 status AFTER=0x%04x "
			    "(PHYDET=%d LINK=%d)\n",
			    data, !!(data & 0x1000), !!(data & 0x0800));
		}
	}

	{
#define PSC_CTRL_REG	0x01
#define PSC_FORCED_SPD	(1 << 13)
#define PSC_FC_ON	(1 << 7)
#define PSC_FORCED_FC	(1 << 6)
#define PSC_LINK_UP	(1 << 5)
#define PSC_FORCED_LINK	(1 << 4)
#define PSC_FULL_DPX	(1 << 3)
#define PSC_FORCED_DPX	(1 << 2)
#define PSC_SPD_1000	0x02

		u16 psc_before, psc_after;
		u16 psc_val;

		igb_vc_intmdio_read(hw, (0x14 << 5) | PSC_CTRL_REG,
		    &psc_before);

		/* First force link down to allow speed change */
		psc_val = PSC_FORCED_LINK;  /* ForceLink=1, LinkUp=0 */
		igb_vc_intmdio_write(hw, (0x14 << 5) | PSC_CTRL_REG,
		    psc_val);

		/* Now set full config and bring link up */
		psc_val = PSC_FORCED_SPD | PSC_SPD_1000 |
		    PSC_FC_ON | PSC_FORCED_FC |
		    PSC_LINK_UP | PSC_FORCED_LINK |
		    PSC_FULL_DPX | PSC_FORCED_DPX;
		igb_vc_intmdio_write(hw, (0x14 << 5) | PSC_CTRL_REG,
		    psc_val);

		igb_vc_intmdio_read(hw, (0x14 << 5) | PSC_CTRL_REG,
		    &psc_after);
		printf("igb_vc: CPU port4 PSC_CTRL: before=0x%04x "
		    "after=0x%04x\n", psc_before, psc_after);
	}


	printf("igb_vc: SGMII forced link: CTRL=0x%08x LCTL=0x%08x "
	    "LSTAT=0x%08x\n", ctrl, lctl,
	    E1000_READ_REG(hw, E1000_PCS_LSTAT));

	return (E1000_SUCCESS);
}

/*
 * Configure 88E1514 LEDs for VeloCloud Edge boards.
 * From Linux igb_vc_setup_m88e1514(), Page 3 registers.
 *
 * Must be called AFTER e1000_initialize_M88E1512_phy() which does
 * a PHY soft reset that clears LED configuration.
 */
void igb_vc_configure_leds(struct e1000_hw *hw);
void
igb_vc_configure_leds(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	u16 page;

	/* Only for VeloCloud WAN ports using bitbang MDIO */
	if (phy->id != M88E1512_E_PHY_ID)
		return;
	if (phy->ops.read_reg != igb_vc_mdiobb_read)
		return;

	/* Save current page, write LED config, restore */
	if (phy->ops.read_reg(hw, 0x16, &page) != E1000_SUCCESS)
		return;

	phy->ops.write_reg(hw, 0x16, 3);      /* Page 3 */
	phy->ops.write_reg(hw, 16, 0x0817);   /* LED Function Control */
	phy->ops.write_reg(hw, 17, 0x0005);   /* LED Polarity Control */
	phy->ops.write_reg(hw, 0x16, page);   /* Restore page */
}
/*
 * ===== Section 6: Post-reset switch verification =====
 */

/*
 * Low-level MDIC read with explicit EXT_MDIO control.
 * Bypasses igb_vc_intmdio_read for diagnostic purposes.
 * Returns 0 on success, -1 timeout, -2 no device ACK.
 */
static s32
igb_vc_diag_mdic_read(struct e1000_hw *hw, u32 dev_addr, u32 reg,
    int ext_mdio, u16 *data)
{
	u32 mdic, mdicnfg, saved;
	int i;

	/*
	 * Preserve COM_MDIO and restore MDICNFG afterwards.  On the I354 the
	 * MDIO pins can be shared with LAN 0 and the NVM decides that per
	 * port; building MDICNFG from scratch drops the bit and makes the scan
	 * report devices that are not really there.  The scan also leaves
	 * MDICNFG pointing at the last address probed, and later switch
	 * access depends on the value igb_vc_intmdio_read() programmed, so
	 * put it back on every exit path.
	 */
	saved = E1000_READ_REG(hw, E1000_MDICNFG);
	mdicnfg = saved & E1000_MDICNFG_COM_MDIO;
	mdicnfg |= (dev_addr << E1000_MDIC_PHY_SHIFT);
	if (ext_mdio)
		mdicnfg |= E1000_MDICNFG_EXT_MDIO;
	E1000_WRITE_REG(hw, E1000_MDICNFG, mdicnfg);
	E1000_WRITE_FLUSH(hw);

	mdic = E1000_MDIC_OP_READ | (reg << E1000_MDIC_REG_SHIFT);
	E1000_WRITE_REG(hw, E1000_MDIC, mdic);

	for (i = 0; i < 100; i++) {
		DELAY(10);
		mdic = E1000_READ_REG(hw, E1000_MDIC);
		if (mdic & E1000_MDIC_READY)
			break;
	}
	if (!(mdic & E1000_MDIC_READY)) {
		E1000_WRITE_REG(hw, E1000_MDICNFG, saved);
		return -1;
	}
	if (mdic & E1000_MDIC_ERROR) {
		E1000_WRITE_REG(hw, E1000_MDICNFG, saved);
		return -2;
	}

	*data = (u16)(mdic & 0xffff);
	E1000_WRITE_REG(hw, E1000_MDICNFG, saved);
	return 0;
}

/*
 * Post-reset MDIO bus scan.
 * Called from em_if_attach_post() after em_reset().
 * Scans all 32 MDIO device addresses with both EXT_MDIO settings.
 */
void igb_vc_verify_switch(struct e1000_hw *hw);
void
igb_vc_verify_switch(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	u32 status, ctrl, ctrl_ext, mdicnfg;
	u32 pcs_cfg, pcs_lctl, pcs_lstat;
	u32 rctl, tctl, connsw;
	u8 func;
	u16 data;
	s32 ret;
	int ext, addr;

	if (phy->ops.read_reg != igb_vc_intmdio_read)
		return;

	/*
	 * Bring-up diagnostics only.  Ungated, this ran on every attach of
	 * every switch port: 64 MDIO transactions and ~70 lines of console
	 * output per boot, each one reprogramming MDICNFG.
	 */
	if (!igb_vc_verbose())
		return;

	status = E1000_READ_REG(hw, E1000_STATUS);
	func = (u8)((status & E1000_STATUS_FUNC_MASK) >>
		     E1000_STATUS_FUNC_SHIFT);
	ctrl = E1000_READ_REG(hw, E1000_CTRL);
	ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
	mdicnfg = E1000_READ_REG(hw, E1000_MDICNFG);
	pcs_cfg = E1000_READ_REG(hw, E1000_PCS_CFG0);
	pcs_lctl = E1000_READ_REG(hw, E1000_PCS_LCTL);
	pcs_lstat = E1000_READ_REG(hw, E1000_PCS_LSTAT);
	rctl = E1000_READ_REG(hw, E1000_RCTL);
	tctl = E1000_READ_REG(hw, E1000_TCTL);
	connsw = E1000_READ_REG(hw, E1000_CONNSW);

	printf("igb_vc: func%u DIAG: STATUS=0x%08x CTRL=0x%08x\n",
	    func, status, ctrl);
	printf("igb_vc: func%u DIAG: CTRL_EXT=0x%08x CONNSW=0x%08x\n",
	    func, ctrl_ext, connsw);
	printf("igb_vc: func%u DIAG: RCTL=0x%08x TCTL=0x%08x\n",
	    func, rctl, tctl);
	printf("igb_vc: func%u DIAG: MDICNFG=0x%08x (EXT=%d COM=%d PHY=%u)\n",
	    func, mdicnfg,
	    !!(mdicnfg & E1000_MDICNFG_EXT_MDIO),
	    !!(mdicnfg & E1000_MDICNFG_COM_MDIO),
	    (mdicnfg >> E1000_MDIC_PHY_SHIFT) & 0x1f);
	printf("igb_vc: func%u DIAG: PCS_CFG=0x%08x LCTL=0x%08x LSTAT=0x%08x\n",
	    func, pcs_cfg, pcs_lctl, pcs_lstat);

	if (hw->phy.ops.acquire)
		hw->phy.ops.acquire(hw);

	for (ext = 0; ext <= 1; ext++) {
		int found = 0;
		printf("igb_vc: func%u SCAN EXT_MDIO=%d:", func, ext);
		for (addr = 0; addr < 32; addr++) {
			data = 0;
			ret = igb_vc_diag_mdic_read(hw, addr, 3,
			    ext, &data);
			if (ret == 0) {
				printf(" dev%02x=0x%04x", addr, data);
				found++;
			}
		}
		if (!found)
			printf(" (no devices)");
		printf("\n");
	}

	/*
	 * Read switch port registers (only meaningful after e6000sw attach,
	 * but dump defaults here for baseline comparison).
	 *
	 * 88E6176 multi-chip base 0x10:
	 *   port 0 = dev 0x10, port 4 (CPU) = dev 0x14
	 *   Reg 0: Port Status
	 *   Reg 4: Port Control (bits [1:0] = state: 0=Dis 3=Fwd)
	 *   Reg 6: Port Based VLAN Map
	 *   Reg 7: Default VLAN ID & Priority
	 */
	{
		int p;
		u16 pstat, pctrl, pvlan, pvid;
		/*
		 * All seven: ports 0-3 are the jacks, and until now this dump
		 * never read them - the only port state ever recorded from this
		 * box came from 0/4/5/6.
		 */
		int ports[] = {0, 1, 2, 3, 4, 5, 6};
		int nports = 7;

		printf("igb_vc: func%u SWITCH PORT REGS:\n", func);
		for (p = 0; p < nports; p++) {
			int dev = 0x10 + ports[p];
			igb_vc_diag_mdic_read(hw, dev, 0, 1, &pstat);
			igb_vc_diag_mdic_read(hw, dev, 4, 1, &pctrl);
			igb_vc_diag_mdic_read(hw, dev, 6, 1, &pvlan);
			igb_vc_diag_mdic_read(hw, dev, 7, 1, &pvid);
			printf("igb_vc:   port%d(dev%02x): "
			    "Status=0x%04x Ctrl=0x%04x "
			    "VLANMap=0x%04x PVID=0x%04x "
			    "state=%d\n",
			    ports[p], dev, pstat, pctrl,
			    pvlan, pvid,
			    pctrl & 0x3);
		}
	}

	/* Dump SCTL (SerDes Control) for loopback state */
	printf("igb_vc: func%u DIAG: SCTL=0x%08x\n",
	    func, E1000_READ_REG(hw, E1000_SCTL));

	if (hw->phy.ops.release)
		hw->phy.ops.release(hw);
}

/* Declare dependency so vc_mdio symbols are available at load time. The first
 * argument is only a C identifier for the metadata uniquifier - the dependency
 * recorded in the .ko is "vc_mdio" either way (sys/sys/module.h:119). */
MODULE_DEPEND(vce520b, vc_mdio, 1, 1, 1);

