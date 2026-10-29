/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 VeloCloud/VMware
 *
 * GPIO bitbang MDIO bus driver for VeloCloud Edge 520B.
 *
 * SC13 = MDIO data (bidirectional), SC14 = MDC clock (output)
 * PHY addr 0 = WAN0, PHY addr 1 = WAN1 (both 88E1514)
 *
 * Uses vc_gpio_pin_* from vc_pcu_gpio for GPIO access.
 * Implements IEEE 802.3 Clause 22 MDIO transactions.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include "vc_pcu_gpio.h"
#include "vc_mdio.h"

/* GPIO pin numbers */
#define	VC_MDIO_PIN_DATA	13
#define	VC_MDIO_PIN_CLK		14

/* Timing */
#define	MDIO_HALF_PERIOD_US	1

/* MDIO frame */
#define	MDIO_PREAMBLE_BITS	32

static struct mtx	mdio_mtx;
static int		mdio_attached;

/* ---- Low-level GPIO helpers ---- */

static __inline void
mdc_set(int val)
{
	vc_gpio_pin_set(VC_MDIO_PIN_CLK, val);
}

static __inline void
mdio_set(int val)
{
	vc_gpio_pin_set(VC_MDIO_PIN_DATA, val);
}

static __inline int
mdio_get(void)
{
	unsigned int val;
	vc_gpio_pin_get(VC_MDIO_PIN_DATA, &val);
	return (val != 0);
}

static __inline void
mdio_dir_out(void)
{
	vc_gpio_pin_setflags(VC_MDIO_PIN_DATA, VC_GPIO_OUTPUT);
}

static __inline void
mdio_dir_in(void)
{
	vc_gpio_pin_setflags(VC_MDIO_PIN_DATA, VC_GPIO_INPUT);
}

static __inline void
mdio_clock(void)
{
	DELAY(MDIO_HALF_PERIOD_US);
	mdc_set(1);
	DELAY(MDIO_HALF_PERIOD_US);
	mdc_set(0);
}

static void
mdio_send_bits(uint32_t val, int nbits)
{
	int i;

	for (i = nbits - 1; i >= 0; i--) {
		mdio_set((val >> i) & 1);
		mdio_clock();
	}
}

static uint16_t
mdio_recv_bits(int nbits)
{
	uint16_t val = 0;
	int i;

	for (i = nbits - 1; i >= 0; i--) {
		DELAY(MDIO_HALF_PERIOD_US);
		mdc_set(1);
		val |= (mdio_get() << i);
		DELAY(MDIO_HALF_PERIOD_US);
		mdc_set(0);
	}
	return (val);
}

/* ---- MDIO Clause 22 ---- */

static int
mdio_read(int phy, int reg)
{
	uint16_t data;

	mtx_lock(&mdio_mtx);

	mdio_dir_out();
	mdc_set(0);

	/* Preamble */
	mdio_send_bits(0xFFFFFFFF, MDIO_PREAMBLE_BITS);

	/* Start(01) + Read(10) = 0x06 */
	mdio_send_bits(0x06, 4);

	/* PHY addr + Reg addr */
	mdio_send_bits(phy & 0x1F, 5);
	mdio_send_bits(reg & 0x1F, 5);

	/* Turnaround */
	mdio_dir_in();
	mdio_clock();

	/* Read 16 data bits */
	data = mdio_recv_bits(16);

	mdio_clock();
	mdio_dir_in();

	mtx_unlock(&mdio_mtx);

	return ((int)data);
}

static int
mdio_write(int phy, int reg, int val)
{
	mtx_lock(&mdio_mtx);

	mdio_dir_out();
	mdc_set(0);

	/* Preamble */
	mdio_send_bits(0xFFFFFFFF, MDIO_PREAMBLE_BITS);

	/* Start(01) + Write(01) = 0x05 */
	mdio_send_bits(0x05, 4);

	/* PHY addr + Reg addr */
	mdio_send_bits(phy & 0x1F, 5);
	mdio_send_bits(reg & 0x1F, 5);

	/* Turnaround: 10 */
	mdio_send_bits(0x02, 2);

	/* Data */
	mdio_send_bits(val & 0xFFFF, 16);

	mdio_dir_in();

	mtx_unlock(&mdio_mtx);

	return (0);
}

/* ---- Exported API ---- */

int
vc_mdio_bus_read(int phy, int reg)
{
	if (!mdio_attached)
		return (-1);
	return (mdio_read(phy, reg));
}

int
vc_mdio_bus_write(int phy, int reg, int val)
{
	if (!mdio_attached)
		return (-1);
	return (mdio_write(phy, reg, val));
}

/* ---- Module lifecycle ---- */

static int
vc_mdio_load(void)
{
	int id1, id2;

	mtx_init(&mdio_mtx, "vc_mdio", NULL, MTX_DEF);

	/* Configure pins: MDC output, MDIO input */
	vc_gpio_pin_setflags(VC_MDIO_PIN_CLK, VC_GPIO_OUTPUT);
	vc_gpio_pin_set(VC_MDIO_PIN_CLK, 0);
	vc_gpio_pin_setflags(VC_MDIO_PIN_DATA, VC_GPIO_INPUT);

	mdio_attached = 1;

	/* Probe PHY IDs */
	id1 = mdio_read(0, 2);
	id2 = mdio_read(0, 3);
	printf("vc_mdio: PHY0 ID: 0x%04x:0x%04x\n", id1, id2);

	id1 = mdio_read(1, 2);
	id2 = mdio_read(1, 3);
	printf("vc_mdio: PHY1 ID: 0x%04x:0x%04x\n", id1, id2);

	printf("vc_mdio: MDIO bus ready (SC13=data, SC14=clock)\n");
	return (0);
}

static int
vc_mdio_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
		return (vc_mdio_load());
	case MOD_UNLOAD:
		mdio_attached = 0;
		vc_gpio_pin_setflags(VC_MDIO_PIN_DATA, VC_GPIO_INPUT);
		mtx_destroy(&mdio_mtx);
		printf("vc_mdio: unloaded\n");
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t vc_mdio_mod = {
	"vc_mdio",
	vc_mdio_modevent,
	NULL
};

DECLARE_MODULE(vc_mdio, vc_mdio_mod, SI_SUB_DRIVERS, SI_ORDER_THIRD);
MODULE_DEPEND(vc_mdio, vc_pcu_gpio, 1, 1, 1);
MODULE_VERSION(vc_mdio, 1);
