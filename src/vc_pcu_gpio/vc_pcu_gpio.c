/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 VeloCloud/VMware
 *
 * Intel Atom C2000 (Rangeley) PCU GPIO controller — simple kmod.
 *
 * Accesses GPIO pins through I/O port registers (GBASE from PCI config).
 * Exports vc_gpio_pin_{get,set,setflags}() for vc_board and vc_mdio.
 *
 * Two GPIO wells:
 *   SC (Core):    GBASE + 0x00, pins 0-30  (31 pins)
 *   SUS (Suspend): GBASE + 0x80, pins 0-27  (28 pins)
 *
 * Pin numbering: SC pins 0..30, SUS pins 31..58.
 *
 * HARDWARE BUG: GP_LVL readback broken for outputs — must cache writes.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <machine/pci_cfgreg.h>

#include "vc_pcu_gpio.h"

/* PCI config */
#define	REG_LPC_GBASE		0x48
#define	REG_LPC_GBASE_EN	0x02
#define	REG_LPC_GBASE_IO	0x01
#define	REG_LPC_GBASE_MASK	0xff00u

/* Well offsets */
#define	PCU_GPIO_SC_OFF		0x00
#define	PCU_GPIO_SUS_OFF	0x80

/* Register offsets within each well */
#define	PCU_USE_SEL		0x00
#define	PCU_IO_SEL		0x04
#define	PCU_GP_LVL		0x08

/* Pin counts */
#define	PCU_SC_NPINS		31
#define	PCU_SUS_NPINS		28
#define	PCU_TOTAL_PINS		(PCU_SC_NPINS + PCU_SUS_NPINS)
#define	PCU_SUS_PIN_BASE	PCU_SC_NPINS

/* PCI device ID */
#define	PCI_DEVICE_ID_ATOM_C2000_PCU	0x1f38

/*
 * Edge 520B rev B GPIO configuration from Linux gpio-pcu.c.
 *
 * SMB bus pin assignments (from velocloud.h):
 *   SMB0: SC8=CLK, SC9=DATA  (firmware recovery, not used here)
 *   SMB1: SC12=CLK, SC11=DATA (PCA9557 I2C bus) ← we use this
 *   SMB2: SC14=CLK, SC13=DATA (MDIO bitbang)
 *
 * Linux rev B clears SMB1 from GPIO (hands to hardware SMBus controller).
 * We override this: keep SMB1 in GPIO mode for I2C bitbang, since
 * FreeBSD doesn't have the velocloud SMBus setup code.
 */
#define	EDGE5X0_BB_SMB1		((1u << 11) | (1u << 12))
#define	EDGE5X0_BB_SMB2		((1u << 13) | (1u << 14))

#define	EDGE5X0B_SC_MASK	((1u<<4) | (1u<<5) | (1u<<15) | \
				 (1u<<17) | (1u<<18) | \
				 EDGE5X0_BB_SMB1 | EDGE5X0_BB_SMB2)
#define	EDGE5X0B_SC_CLEAR	0u	/* Keep SMB1 in GPIO mode for bitbang */
#define	EDGE5X0B_SC_IOSEL	((1u<<4) | (1u<<5) | (1u<<15) | \
				 (1u<<17) | (1u<<18) | \
				 EDGE5X0_BB_SMB1 | EDGE5X0_BB_SMB2)
#define	EDGE5X0B_SC_OUT		0u

#define	EDGE5X0B_SUS_MASK	((1u<<2) | (1u<<6) | (1u<<7) | \
				 (1u<<17) | (1u<<18))
#define	EDGE5X0B_SUS_CLEAR	((1u<<24) | (1u<<25) | (1u<<26) | (1u<<27))
#define	EDGE5X0B_SUS_IOSEL	((1u<<17) | (1u<<18))
#define	EDGE5X0B_SUS_OUT	(1u<<2)

/* Module state — single instance, no driver framework */
static struct mtx	gpio_mtx;
static bus_space_tag_t	gpio_bst;
static bus_space_handle_t gpio_bsh;
static int		gpio_attached;

/* Cached output values (GP_LVL readback bug workaround) */
static uint32_t		out_sc;
static uint32_t		out_sus;

/* Pin masks — which pins we control */
static uint32_t		mask_sc;
static uint32_t		mask_sus;

#define	GPIO_READ(off)		bus_space_read_4(gpio_bst, gpio_bsh, (off))
#define	GPIO_WRITE(off, val)	bus_space_write_4(gpio_bst, gpio_bsh, (off), (val))

/* ---- Pin resolution ---- */

static int
pin_to_well(uint32_t pin, bus_size_t *base, uint32_t *bit,
    uint32_t **out_cache, uint32_t *pmask)
{
	if (pin < PCU_SC_NPINS) {
		*base = PCU_GPIO_SC_OFF;
		*bit = 1u << pin;
		*out_cache = &out_sc;
		*pmask = mask_sc;
		return (0);
	}
	pin -= PCU_SUS_PIN_BASE;
	if (pin < PCU_SUS_NPINS) {
		*base = PCU_GPIO_SUS_OFF;
		*bit = 1u << pin;
		*out_cache = &out_sus;
		*pmask = mask_sus;
		return (0);
	}
	return (EINVAL);
}

/* ---- Exported API ---- */

int
vc_gpio_pin_get(uint32_t pin, unsigned int *val)
{
	bus_size_t base;
	uint32_t bit, *out_cache, pmask, dir, lvl;

	if (!gpio_attached)
		return (ENXIO);
	if (pin_to_well(pin, &base, &bit, &out_cache, &pmask))
		return (EINVAL);

	mtx_lock(&gpio_mtx);
	lvl = GPIO_READ(base + PCU_GP_LVL);
	dir = GPIO_READ(base + PCU_IO_SEL);
	/* Hardware for inputs, cached for outputs */
	lvl = (lvl & dir) | (*out_cache & ~dir);
	mtx_unlock(&gpio_mtx);

	*val = (lvl & bit) ? 1 : 0;
	return (0);
}

int
vc_gpio_pin_set(uint32_t pin, unsigned int val)
{
	bus_size_t base;
	uint32_t bit, *out_cache, pmask, lvl;

	if (!gpio_attached)
		return (ENXIO);
	if (pin_to_well(pin, &base, &bit, &out_cache, &pmask))
		return (EINVAL);

	mtx_lock(&gpio_mtx);
	lvl = *out_cache;
	if (val)
		lvl |= bit;
	else
		lvl &= ~bit;
	*out_cache = lvl;
	GPIO_WRITE(base + PCU_GP_LVL, lvl);
	mtx_unlock(&gpio_mtx);

	return (0);
}

int
vc_gpio_pin_setflags(uint32_t pin, int dir)
{
	bus_size_t base;
	uint32_t bit, *out_cache, pmask, iosel;

	if (!gpio_attached)
		return (ENXIO);
	if (pin_to_well(pin, &base, &bit, &out_cache, &pmask))
		return (EINVAL);

	mtx_lock(&gpio_mtx);
	iosel = GPIO_READ(base + PCU_IO_SEL);
	if (dir == VC_GPIO_INPUT)
		iosel |= bit;
	else
		iosel &= ~bit;
	GPIO_WRITE(base + PCU_IO_SEL, iosel);
	mtx_unlock(&gpio_mtx);

	return (0);
}

/* ---- Well initialization ---- */

static void
init_well(const char *name, bus_size_t base,
    uint32_t mask, uint32_t clear, uint32_t iosel, uint32_t out)
{
	uint32_t val, bad;

	/* Fix USE_SEL */
	val = GPIO_READ(base + PCU_USE_SEL);
	printf("vc_pcu_gpio: %s: BIOS use_sel=0x%08x\n", name, val);
	bad = ((val & mask) ^ mask) | (val & clear);
	if (bad) {
		val |= mask;
		val &= ~clear;
		GPIO_WRITE(base + PCU_USE_SEL, val);
		printf("vc_pcu_gpio: %s: fixed use_sel=0x%08x\n", name, val);
	}

	/* Fix IO_SEL */
	val = GPIO_READ(base + PCU_IO_SEL);
	printf("vc_pcu_gpio: %s: BIOS io_sel=0x%08x\n", name, val);
	bad = (val ^ iosel) & mask;
	if (bad) {
		val = (val & ~mask) | (iosel & mask);
		GPIO_WRITE(base + PCU_IO_SEL, val);
		printf("vc_pcu_gpio: %s: fixed io_sel=0x%08x\n", name, val);
	}

	/* Set initial output values */
	GPIO_WRITE(base + PCU_GP_LVL, out);

	printf("vc_pcu_gpio: %s: mask=0x%08x iosel=0x%08x out=0x%08x\n",
	    name, mask, iosel, out);
}

/* ---- Module lifecycle ---- */

static int
vc_pcu_gpio_load(void)
{
	u_int32_t	vendor, dev_id;
	uint32_t	gbase;

	/*
	 * This module initialises at SI_SUB_DRIVERS, before newbus enumerates
	 * PCI at SI_SUB_CONFIGURE, so pci_find_bsf() can never succeed here.
	 * Read the PCU directly out of type-1 configuration space (CF8/CFC);
	 * 00:1f.0 is a fixed address on Atom C2000.
	 */
	vendor = pci_cfgregread(0, 0, 31, 0, PCIR_VENDOR, 2);
	dev_id = pci_cfgregread(0, 0, 31, 0, PCIR_DEVICE, 2);
	if (vendor != 0x8086 || dev_id != PCI_DEVICE_ID_ATOM_C2000_PCU) {
		printf("vc_pcu_gpio: PCI 00:1f.0 is not Atom C2000 PCU "
		    "(vendor=0x%04x device=0x%04x)\n", vendor, dev_id);
		return (ENXIO);
	}

	/* Read GBASE from PCI config */
	gbase = pci_cfgregread(0, 0, 31, 0, REG_LPC_GBASE, 4);
	if ((gbase & (REG_LPC_GBASE_EN | REG_LPC_GBASE_IO)) !=
	    (REG_LPC_GBASE_EN | REG_LPC_GBASE_IO)) {
		printf("vc_pcu_gpio: GBASE not enabled (raw=0x%08x)\n", gbase);
		return (ENXIO);
	}
	gbase &= REG_LPC_GBASE_MASK;
	printf("vc_pcu_gpio: GPIO I/O base=0x%04x\n", gbase);

	/* Map I/O ports */
	gpio_bst = X86_BUS_SPACE_IO;
	if (bus_space_map(gpio_bst, gbase, 0x100, 0, &gpio_bsh) != 0) {
		printf("vc_pcu_gpio: failed to map I/O at 0x%04x\n", gbase);
		return (ENXIO);
	}

	mtx_init(&gpio_mtx, "vc_pcu_gpio", NULL, MTX_DEF);

	/* Edge 520B rev B configuration */
	mask_sc = EDGE5X0B_SC_MASK;
	mask_sus = EDGE5X0B_SUS_MASK;
	out_sc = EDGE5X0B_SC_OUT;
	out_sus = EDGE5X0B_SUS_OUT;

	/* Initialize GPIO wells */
	init_well("SC", PCU_GPIO_SC_OFF,
	    EDGE5X0B_SC_MASK, EDGE5X0B_SC_CLEAR,
	    EDGE5X0B_SC_IOSEL, EDGE5X0B_SC_OUT);
	init_well("SUS", PCU_GPIO_SUS_OFF,
	    EDGE5X0B_SUS_MASK, EDGE5X0B_SUS_CLEAR,
	    EDGE5X0B_SUS_IOSEL, EDGE5X0B_SUS_OUT);

	gpio_attached = 1;
	printf("vc_pcu_gpio: ready: %d SC + %d SUS = %d GPIO pins\n",
	    PCU_SC_NPINS, PCU_SUS_NPINS, PCU_TOTAL_PINS);
	return (0);
}

static void
vc_pcu_gpio_unload(void)
{
	gpio_attached = 0;
	bus_space_unmap(gpio_bst, gpio_bsh, 0x100);
	mtx_destroy(&gpio_mtx);
	printf("vc_pcu_gpio: unloaded\n");
}

static int
vc_pcu_gpio_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
		return (vc_pcu_gpio_load());
	case MOD_UNLOAD:
		vc_pcu_gpio_unload();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t vc_pcu_gpio_mod = {
	"vc_pcu_gpio",
	vc_pcu_gpio_modevent,
	NULL
};

DECLARE_MODULE(vc_pcu_gpio, vc_pcu_gpio_mod, SI_SUB_DRIVERS, SI_ORDER_FIRST);
MODULE_VERSION(vc_pcu_gpio, 1);
