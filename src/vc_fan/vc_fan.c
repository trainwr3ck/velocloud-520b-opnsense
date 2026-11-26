/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 the vce520b authors.
 *
 * vc_fan: EMC2104 fan access and kernel-side fail-safes for a VeloCloud
 * Edge 520 / 540 rev B running FreeBSD / OPNsense.
 *
 * No code is copied from vc_board.c or any other source; the bit-bang I2C
 * below is a fresh implementation over the exported vc_pcu_gpio pin API.
 *
 * edge520b profile: on/off only (bit 7 of the 0x1c expander, 38 Hz PWM); every
 * fail-safe and module load set bit 7 to input unconditionally.
 *
 * LOADING: only by /usr/local/etc/rc.syshook.d/early/06-velo-fan or by
 * `velo5x0-fan load`, both of which require vc_pcu_gpio AND vc_board to be
 * loaded already. The module itself also refuses to load unless vc_board has
 * already run (hw.velocloud.board_name exists). It depends ONLY on
 * vc_pcu_gpio, so loading it can never pull vc_board in (vc_board's
 * load handler pulses the 0x18 resets). NEVER list it in loader.conf.
 * NEVER kldunload vc_board while vc_fan is loaded: nothing pins it now,
 * and a reload of vc_board pulses 0x18 and bit-bangs the same two pins
 * without vc_fan's lock.
 *
 * Why a kernel module: on FreeBSD the board I2C bus (SC11 = SDA, SC12 = SCL)
 * exists only as static bit-bang code inside vc_board.ko. There is no
 * /dev/iic for it, and userspace bit-banging through /dev/io would race the
 * kernel: vc_mdio bit-bangs SC13/SC14 in the same SC-well GP_LVL register
 * (polled every second by e6000sw), and that register's output levels cannot
 * be read back (vc_pcu_gpio keeps a software cache). Going through
 * vc_gpio_pin_*() keeps every pin change under vc_pcu_gpio's mutex and cache,
 * so MDIO and this I2C never clobber each other's pins.
 *
 * sysctl hw.vc_fan.* (every handler that touches the bus needs PRIV_DRIVER):
 *   identified   RO  1 if 0xfd read 0x1d and 0xfe read 0x5d (twice each) at load
 *   chip_id      RO  0xfd as read at load (-1 = no answer)
 *   duty         RW  0x40, 0..255. Writes need identified. A write below 255
 *                    is refused (EPERM) unless the watchdog is armed,
 *                    and refused (EIO) while a watchdog full write is failing.
 *   tach         RO  raw count ((0x4e << 5) | (0x4f >> 3))
 *   spinup_cfg   RO  0x46
 *   configure    WO  1: open-loop recipe; and, if pins_enable, the 0x1c pins
 *   watchdog_s   RW  0 = disarmed, 5..120 = armed. Writing 0 while armed
 *                    first writes and verifies 255; if that fails the write
 *                    returns EIO and the watchdog STAYS armed.
 *   pet          WO  1: restart the period. EPERM when disarmed.
 *   wd_fired     RO  watchdog fail-safe events since load
 *   wd_failing   RO  1 while a watchdog full write has not verified; retried
 *                    every 5 s
 *   wd_retries   RO  retries of a failing watchdog full write
 *   bus_stuck    RO  transfers refused because SDA stayed low
 *   pins_enable  RW  may the board's 0x1c fan pins be written at configure.
 *                    Default 1 on both boards;
 *                    tunable hw.vc_fan.pins_enable=0 opts out. The pin MASK
 *                    is per board: edge540b bits 6+7 (6 low, 7 high);
 *                    edge520b bit 7 ONLY (driven high); bit 6 untouched there
 *   board        RO  "edge520b" or "edge540b"
 *   pwm_base     RW  -1 = recipe; else PWM_BASE1 written at configure (0x2b 1:0)
 *   pwm_divide   RW  0 = recipe; else 0x41 written at configure (1..255)
 *   pwm_polarity RW  -1 = recipe; else 0x2a bit 0 written at configure
 *   pins_release WO  1: put the mask bits of CFG, then OUT, back to the values
 *                    read before the last configure pin write
 *   pins_failsafe WO 1: edge520b: set CFG bit 7 = 1 (input) unconditionally,
 *                    the hardware fail-safe to full; no-op on edge540b
 *   pins_rel_failing RO 1 while an edge520b fail-safe release has not
 *                    succeeded (retried by the watchdog task)
 *   dump         RO  (hidden from sysctl -a) EMC2104 regs + 0x1c IN/OUT/POL/CFG
 *                    and lvl = IN ^ POL, the actual pin levels
 *
 * Fail-safes (numbered as in docs/fan.md): F6 load, F7 shutdown_pre_sync (before any
 * crash dump) and shutdown_final, F8 unload (EBUSY if not verified),
 * F4/F5 the watchdog, F9 the sub-full refusal.
 *
 * Board rules enforced here:
 *   - 0x18 is never addressed (vcf_xfer refuses it); vc_board is never
 *     loaded by this module (no MODULE_DEPEND on it);
 *   - 0x1c: only OUT (1) or CFG (3), read twice and the reads must agree,
 *     only the board's pin_mask() bits change (edge540b 6+7, edge520b 7
 *     only), read back twice and bits 0/1 checked; POL never written. The
 *     fan pins are DRIVEN only at configure (and on edge520b only while 0x40
 *     reads 255); on edge520b every fail-safe path and module load set CFG
 *     bit 7 = 1 (input, fan full by hardware), unconditionally;
 *   - no SoC GPIO pin other than SC11/SC12 is touched; SUS2 is in the SUS
 *     well, whose register this module never writes.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/eventhandler.h>
#include <sys/sbuf.h>
#include <sys/kdb.h>

#include "vc_pcu_gpio.h"

#define	VCF_PIN_SDA		11	/* SC11 */
#define	VCF_PIN_SCL		12	/* SC12 */
#define	VCF_HALF_US		5	/* ~100 kHz */

#define	EMC_ADDR		0x2f
#define	EMC_PRODUCT_ID_REG	0xfd
#define	EMC_PRODUCT_ID		0x1d
#define	EMC_MFR_ID_REG		0xfe
#define	EMC_MFR_ID		0x5d	/* SMSC / Microchip */
#define	EMC_PWM_CONFIG		0x2a	/* bit 0 POLARITY1 */
#define	EMC_PWM_BASE		0x2b	/* bits 1:0 PWM_BASE1, 3:2 PWM_BASE2 */
#define	EMC_PWM1_DIVIDE		0x41	/* 1..255 */
#define	EMC_FAN_SETTING		0x40
#define	EMC_FAN_CONFIG1		0x42
#define	EMC_SPINUP_CFG		0x46
#define	EMC_TACH_HI		0x4e
#define	EMC_TACH_LO		0x4f
#define	EMC_LUT_CONFIG		0x50
#define	DUTY_FULL		0xff

#define	PCA_RESETS_ADDR		0x18	/* never addressed */
#define	PCA_FAN_ADDR		0x1c
#define	PCA_IN			0
#define	PCA_OUT			1
#define	PCA_POL			2
#define	PCA_CFG			3
#define	PCA_POE_BITS		0x03
#define	PCA_FAN_BITS		0xc0
#define	PCA_FAN_OUT		0x80	/* bit 7 high (FORCE_12V), bit 6 low */

#define	WD_MIN_S		5
#define	WD_MAX_S		120
#define	WD_RETRY_S		5

enum vcf_board { VCF_BOARD_NONE, VCF_BOARD_520B, VCF_BOARD_540B };

static const struct { uint8_t reg, val; } emc_recipe[] = {
	{ 0x50, 0x00 }, { 0x42, 0x10 }, { 0x2b, 0x0c }, { 0x41, 0x01 },
};
static const int spinup_ms[4] = { 250, 500, 1000, 2000 };

static struct sx		vcf_lock;	/* one I2C transaction at a time */
static int			vcf_identified;
static int			vcf_chip_id = -1;
static int			vcf_pins_enable = -1;	/* -1: board default */
static int			vcf_unloading;	/* also set at shutdown */
static int			vcf_inited;	/* load completed */
static int			vcf_pins_saved;	/* saved_* valid */
static uint8_t			vcf_saved_out, vcf_saved_cfg;
/*
 * PWM profile (datasheet rev 1.78 sections 6.17, 6.18, 6.21):
 * applied by configure AFTER the recipe; -1 / 0 = keep the recipe's value.
 * pwm_base: PWM_BASE1 0=26 kHz 1=19.531 kHz 2=4.882 kHz 3=2.441 kHz;
 * pwm_divide: 0x41, 1..255 (frequency = base / divide);
 * pwm_polarity: 0x2a bit 0.
 */
static int			vcf_pwm_base = -1;
static int			vcf_pwm_divide;
static int			vcf_pwm_polarity = -1;
static enum vcf_board		vcf_board;
/* watchdog: written under vcf_lock; the tick reads them without it (an int
 * and an aligned 64-bit value, single loads on amd64) */
static volatile int		vcf_wd_s;
static volatile sbintime_t	vcf_wd_deadline;
static volatile int		vcf_wd_failing;
static sbintime_t		vcf_wd_next_retry;
static int			vcf_wd_fired;
static int			vcf_wd_retries;
static int			vcf_bus_stuck;
static struct callout		vcf_wd_callout;
static struct task		vcf_wd_task;
static struct taskqueue		*vcf_tq;
static eventhandler_tag		vcf_tag_presync, vcf_tag_final;
static struct sysctl_ctx_list	vcf_ctx;

TUNABLE_INT("hw.vc_fan.pins_enable", &vcf_pins_enable);
static int			vcf_rel_failing;	/* release not yet verified */
static int			vcf_rel_fails;
static uint8_t	pin_mask(void);
TUNABLE_INT("hw.vc_fan.pwm_base", &vcf_pwm_base);
TUNABLE_INT("hw.vc_fan.pwm_divide", &vcf_pwm_divide);
TUNABLE_INT("hw.vc_fan.pwm_polarity", &vcf_pwm_polarity);

/* ---------------------------------------------------------------- waits */

/* Sleep. pause_sbt itself spins with DELAY when cold, in the debugger
 * or with the scheduler stopped. */
static void
vcf_wait_ms(int ms)
{
	pause_sbt("vcfan", SBT_1MS * ms, 0, 0);
}

/* ---------------------------------------------------------------- bit-bang */

/* SDA open drain: '1' = release (input, pulled up), '0' = drive low. */
static void
sda(int high)
{
	if (high)
		vc_gpio_pin_setflags(VCF_PIN_SDA, VC_GPIO_INPUT);
	else {
		vc_gpio_pin_set(VCF_PIN_SDA, 0);
		vc_gpio_pin_setflags(VCF_PIN_SDA, VC_GPIO_OUTPUT);
	}
	DELAY(VCF_HALF_US);
}

static void
scl(int high)
{
	vc_gpio_pin_set(VCF_PIN_SCL, high ? 1 : 0);
	DELAY(VCF_HALF_US);
}

static int
sda_read(void)
{
	unsigned int v = 1;

	vc_gpio_pin_get(VCF_PIN_SDA, &v);
	return (v != 0);
}

static void
i2c_start(void)
{
	sda(1); scl(1); sda(0); scl(0);
}

static void
i2c_stop(void)
{
	sda(0); scl(1); sda(1);
}

/*
 * Returns 0 on ACK, 1 on NAK, 2 if a released ('1') bit read low on the wire
 *. In that case SCL is left low and the caller STOPs before the ACK
 * clock, so the slave discards the byte instead of latching a corrupted one.
 */
static int
i2c_tx(uint8_t b)
{
	int i, nak, bit;

	for (i = 7; i >= 0; i--) {
		bit = (b >> i) & 1;
		sda(bit);
		scl(1);
		if (bit && !sda_read()) {
			scl(0);
			return (2);
		}
		scl(0);
	}
	sda(1);
	scl(1);
	nak = sda_read();
	scl(0);
	return (nak);
}

static uint8_t
i2c_rx(int ack)
{
	int i;
	uint8_t b = 0;

	sda(1);
	for (i = 7; i >= 0; i--) {
		scl(1);
		if (sda_read())
			b |= 1u << i;
		scl(0);
	}
	sda(ack ? 0 : 1);
	scl(1);
	scl(0);
	sda(1);
	return (b);
}

/*
 * Bus recovery: a transfer cut off by a reset or a panic can leave a slave
 * holding SDA low. Release SDA, clock SCL until SDA reads high (at most 9),
 * then STOP. Touches only SC11 and SC12.
 */
static void
vcf_bus_recover(void)
{
	int i;

	sda(1);
	for (i = 0; i < 9 && !sda_read(); i++) {
		scl(0);
		scl(1);
	}
	scl(0);
	i2c_stop();
}

/*
 * with SCL high and SDA released the bus must read idle (SDA high). If it
 * does not, recover once; if it still does not, refuse the transfer. A
 * stuck-low SDA would otherwise read as ACK on every byte and 0x00 on every
 * read.
 */
static int
vcf_bus_idle_check(void)
{
	sda(1);
	scl(1);
	if (sda_read())
		return (0);
	vcf_bus_recover();
	scl(1);
	if (sda_read())
		return (0);
	if (vcf_bus_stuck++ % 100 == 0)
		printf("vc_fan: ALERT SDA held low, I2C transfer refused (%d)\n",
		    vcf_bus_stuck);
	return (EIO);
}

/* SMBus read/write byte data. Caller holds vcf_lock. */
static int
vcf_xfer(uint8_t addr, int rd, uint8_t reg, uint8_t *val)
{
	int err;

	if (addr == PCA_RESETS_ADDR)
		return (EPERM);		/* board rule: 0x18 is never addressed */
	if (!rd && addr == PCA_FAN_ADDR && reg != PCA_OUT && reg != PCA_CFG)
		return (EPERM);
	if ((err = vcf_bus_idle_check()) != 0)
		return (err);
	i2c_start();
	if ((err = i2c_tx(addr << 1)) != 0 || (err = i2c_tx(reg)) != 0) {
		err = (err == 2) ? EIO : ENXIO;
		goto out;
	}
	if (rd) {
		i2c_start();		/* repeated start */
		if ((err = i2c_tx((addr << 1) | 1)) != 0) {
			err = (err == 2) ? EIO : ENXIO;
			goto out;
		}
		*val = i2c_rx(0);
	} else if (i2c_tx(*val) != 0)
		err = EIO;
out:
	i2c_stop();
	return (err);
}

/* Two reads that must agree  (an undriven data byte reads 0xff silently). */
static int
vcf_rd2(uint8_t addr, uint8_t reg, uint8_t *v)
{
	uint8_t a, b;
	int err;

	if ((err = vcf_xfer(addr, 1, reg, &a)) != 0)
		return (err);
	if ((err = vcf_xfer(addr, 1, reg, &b)) != 0)
		return (err);
	if (a != b)
		return (EIO);
	*v = a;
	return (0);
}

static int
emc_rd(uint8_t reg, uint8_t *v)
{
	return (vcf_xfer(EMC_ADDR, 1, reg, v));
}

static int
emc_wr(uint8_t reg, uint8_t v)
{
	if (!vcf_identified)
		return (EPERM);
	return (vcf_xfer(EMC_ADDR, 0, reg, &v));
}

/*
 * 0x1c read-modify-write: two agreeing reads before the write; only bits
 * 6/7 of OUT or CFG; two agreeing reads after it, which must equal what was
 * written, with bits 0/1 equal to the pre-read. Any doubt: no write, or an
 * ALERT if the doubt appears after the write.
 */
static int
pca1c_rmw(uint8_t reg, uint8_t set, uint8_t clr)
{
	uint8_t before, after, check;
	int err;

	if (reg != PCA_OUT && reg != PCA_CFG)
		return (EPERM);
	if ((set | clr) & ~pin_mask())		/* the board's mask, not 6+7 */
		return (EPERM);
	if ((err = vcf_rd2(PCA_FAN_ADDR, reg, &before)) != 0)
		return (err);		/* reads disagree: write nothing */
	after = (before & ~clr) | set;
	if (after == before)
		return (0);
	if ((err = vcf_xfer(PCA_FAN_ADDR, 0, reg, &after)) != 0) {
		printf("vc_fan: ALERT 0x1c reg %d write failed (%d)\n", reg, err);
		return (err);
	}
	if ((err = vcf_rd2(PCA_FAN_ADDR, reg, &check)) != 0 || check != after ||
	    (check & PCA_POE_BITS) != (before & PCA_POE_BITS)) {
		printf("vc_fan: ALERT 0x1c reg %d read-back 0x%02x after writing 0x%02x "
		    "(before 0x%02x, err %d)\n", reg, check, after, before, err);
		return (EIO);
	}
	return (0);
}

static int
pins_allowed(void)
{
	if (vcf_pins_enable >= 0)
		return (vcf_pins_enable != 0);
	return (1);
}

/* The fan pins this board's profile drives. On edge520b only bit 7:
 * measured on an Edge 520 v2, bit 7 driven high is what lets the EMC2104
 * PWM switch the 3-wire fan's supply; bit 6 is left exactly as found. */
static uint8_t
pin_mask(void)
{
	return (vcf_board == VCF_BOARD_520B ? 0x80 : PCA_FAN_BITS);
}

/*
 * check the actual pin levels after a pin write. IN is inverted by POL,
 * so the level is IN ^ POL. For every fan pin (6, 7) that CFG makes an
 * output, the level must equal the OUT bit. Returns 0 when it does.
 */
static int
pca1c_check_levels(void)
{
	uint8_t in, pol, out, cfg, lvl, drv;
	int err;

	if ((err = vcf_rd2(PCA_FAN_ADDR, PCA_IN, &in)) != 0 ||
	    (err = vcf_rd2(PCA_FAN_ADDR, PCA_POL, &pol)) != 0 ||
	    (err = vcf_rd2(PCA_FAN_ADDR, PCA_OUT, &out)) != 0 ||
	    (err = vcf_rd2(PCA_FAN_ADDR, PCA_CFG, &cfg)) != 0)
		return (err);
	lvl = in ^ pol;
	drv = ~cfg & pin_mask();		/* profile pins that are outputs */
	if ((lvl & drv) != (out & drv)) {
		printf("vc_fan: ALERT 0x1c pin levels 0x%02x do not follow OUT 0x%02x on "
		    "driven fan pins 0x%02x (IN 0x%02x POL 0x%02x CFG 0x%02x): contention?\n",
		    lvl, out, drv, in, pol, cfg);
		return (EIO);
	}
	return (0);
}

/*
 * put bits 6/7 of CFG (first: stop driving) and then OUT back to the
 * values read before the last configure pin write. Same read-modify-write
 * rules; bits 0-5 are untouched.
 */
static int
pca1c_release_locked(void)
{
	int err;

	if (!vcf_pins_saved)
		return (ENOENT);
	err = pca1c_rmw(PCA_CFG, vcf_saved_cfg & pin_mask(),
	    ~vcf_saved_cfg & pin_mask());
	if (err == 0)
		err = pca1c_rmw(PCA_OUT, vcf_saved_out & pin_mask(),
		    ~vcf_saved_out & pin_mask());
	if (err == 0) {
		vcf_pins_saved = 0;
		printf("vc_fan: 0x1c fan pins released to OUT 0x%02x CFG 0x%02x (mask 0x%02x)\n",
		    vcf_saved_out & pin_mask(), vcf_saved_cfg & pin_mask(), pin_mask());
	} else
		printf("vc_fan: ALERT 0x1c pin release failed (%d)\n", err);
	return (err);
}

/*
 * The one pin write (configure, pins allowed): save OUT/CFG, set OUT first
 * (latch), then CFG (direction), then check the pin levels; on any failure
 * release at once.
 */
static int
pca1c_apply_locked(void)
{
	uint8_t out, cfg;
	int err;

	if (vcf_board == VCF_BOARD_520B) {
		/* on edge520b driving bit 7 hands the fan to the PWM; only
		 * do it with the duty register verified at full */
		uint8_t d;

		if ((err = vcf_rd2(EMC_ADDR, EMC_FAN_SETTING, &d)) != 0)
			return (err);
		if (d != DUTY_FULL) {
			printf("vc_fan: pin write refused: duty reads 0x%02x, not 0xff\n", d);
			/* NOT EAGAIN: the sysctl layer retries a handler that returns
			 * EAGAIN, which made this a kernel livelock (found by the VM
			 * stub test) */
			return (EBUSY);
		}
	}
	if ((err = vcf_rd2(PCA_FAN_ADDR, PCA_OUT, &out)) != 0 ||
	    (err = vcf_rd2(PCA_FAN_ADDR, PCA_CFG, &cfg)) != 0)
		return (err);
	if (!vcf_pins_saved) {
		vcf_saved_out = out;
		vcf_saved_cfg = cfg;
		vcf_pins_saved = 1;
	}
	err = pca1c_rmw(PCA_OUT, PCA_FAN_OUT & pin_mask(),
	    PCA_FAN_BITS & ~PCA_FAN_OUT & pin_mask());
	if (err == 0)
		err = pca1c_rmw(PCA_CFG, 0, pin_mask());
	if (err == 0)
		err = pca1c_check_levels();
	if (err != 0)
		(void)pca1c_release_locked();
	return (err);
}

/*
 * Write 255, wait the spin-up time, verify by two agreeing reads; up to 3
 * tries. Lock held (or the scheduler stopped). Never touches 0x1c.
 */
static int
vcf_full_q(const char *why, int quiet)
{
	uint8_t cfg = 0x03, v = 0;
	int i, ms;

	if (!vcf_identified) {
		printf("vc_fan: full duty not attempted (%s): chip not identified\n", why);
		return (ENXIO);
	}
	if (emc_rd(EMC_SPINUP_CFG, &cfg) != 0)
		cfg = 0x03;
	ms = spinup_ms[cfg & 0x03] + 100;
	for (i = 1; i <= 3; i++) {
		if (emc_wr(EMC_FAN_SETTING, DUTY_FULL) != 0) {
			vcf_bus_recover();
			continue;
		}
		vcf_wait_ms(ms);
		if (vcf_rd2(EMC_ADDR, EMC_FAN_SETTING, &v) == 0 && v == DUTY_FULL) {
			printf("vc_fan: full duty 0xff verified (%s)\n", why);
			return (0);
		}
	}
	if (!quiet)
		printf("vc_fan: ALERT full duty NOT verified after 3 tries (%s), "
		    "last 0x%02x\n", why, v);
	return (EIO);
}

static int
vcf_full_locked(const char *why)
{
	return (vcf_full_q(why, 0));
}

/*
 * edge520b: with bit 7 released (input, as at power-on) the fan runs at
 * full speed whatever the EMC2104 drives (measured on an Edge 520 v2). So on
 * that board every fail-safe ALSO releases bit 7: a second, independent path
 * to full. Lock held (or scheduler stopped). No effect on edge540b.
 */
static int
vcf_failsafe_pins_locked(const char *why)
{
	int err;

	if (vcf_board != VCF_BOARD_520B)
		return (0);
	/* a fixed target, not a restore: CFG bit 7 = 1 (input) */
	err = pca1c_rmw(PCA_CFG, 0x80, 0);
	if (err == 0) {
		vcf_pins_saved = 0;
		if (vcf_rel_failing || vcf_rel_fails == 0)
			printf("vc_fan: edge520b bit 7 set to input (%s): fan forced full "
			    "by hardware\n", why);
		vcf_rel_failing = 0;
	} else {
		if (vcf_rel_fails++ % 12 == 0)
			printf("vc_fan: ALERT edge520b bit 7 release FAILED (%s, err %d); "
			    "retrying\n", why, err);
		vcf_rel_failing = 1;
	}
	return (err);
}

/* ---------------------------------------------------------------- watchdog */

static void
vcf_pet_locked(void)
{
	vcf_wd_deadline = sbinuptime() + (sbintime_t)vcf_wd_s * SBT_1S;
}

static void
vcf_wd_task_fn(void *arg, int pending)
{
	sx_xlock(&vcf_lock);
	if (vcf_wd_s != 0 && sbinuptime() >= vcf_wd_deadline) {
		vcf_wd_s = 0;		/* disarm: sub-full writes now refused */
		vcf_wd_fired++;
		printf("vc_fan: watchdog: no pet from the daemon; forcing full duty\n");
		(void)vcf_failsafe_pins_locked("kernel watchdog");	/* first */
		vcf_wd_failing = (vcf_full_locked("kernel watchdog") != 0);
		vcf_wd_next_retry = sbinuptime() + WD_RETRY_S * SBT_1S;
	} else if ((vcf_wd_failing || vcf_rel_failing) &&
	    sbinuptime() >= vcf_wd_next_retry) {
		vcf_wd_retries++;
		if (vcf_rel_failing)		/* retry the release too */
			(void)vcf_failsafe_pins_locked("kernel watchdog retry");
		/* retry; alert on the first retry and then once a minute */
		if (vcf_wd_failing)
			vcf_wd_failing = (vcf_full_q("kernel watchdog retry",
			    vcf_wd_retries % (60 / WD_RETRY_S) != 1) != 0);
		vcf_wd_next_retry = sbinuptime() + WD_RETRY_S * SBT_1S;
	}
	sx_xunlock(&vcf_lock);
}

static void
vcf_wd_tick(void *arg)
{
	sbintime_t now = sbinuptime();

	if ((vcf_wd_s != 0 && now >= vcf_wd_deadline) ||
	    ((vcf_wd_failing || vcf_rel_failing) && now >= vcf_wd_next_retry))
		taskqueue_enqueue(vcf_tq, &vcf_wd_task);
	callout_reset(&vcf_wd_callout, hz, vcf_wd_tick, NULL);
}

/* ---------------------------------------------------------------- sysctl */

static int
vcf_priv(struct sysctl_req *req)
{
	return (req->td != NULL ? priv_check(req->td, PRIV_DRIVER) : 0);
}

static int
sysctl_duty(SYSCTL_HANDLER_ARGS)
{
	uint8_t v8;
	int v, err;

	if ((err = vcf_priv(req)) != 0)
		return (err);
	sx_xlock(&vcf_lock);
	err = vcf_rd2(EMC_ADDR, EMC_FAN_SETTING, &v8);	/* two agreeing reads */
	v = err ? -1 : v8;
	sx_xunlock(&vcf_lock);
	if (err != 0 && req->newptr == NULL)
		return (err);
	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (v < 0 || v > 255)
		return (EINVAL);
	sx_xlock(&vcf_lock);
	if (vcf_unloading)
		err = EBUSY;
	else if (!vcf_identified)
		err = EPERM;
	else if (v != DUTY_FULL && vcf_wd_failing)
		err = EIO;		/* a full write is failing */
	else if (v != DUTY_FULL && vcf_wd_s == 0)
		err = EPERM;		/* F9: no low duty without a live loop */
	else if ((err = emc_wr(EMC_FAN_SETTING, (uint8_t)v)) == 0 && vcf_wd_s != 0)
		vcf_pet_locked();
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_rdreg(SYSCTL_HANDLER_ARGS)
{
	uint8_t hi = 0, lo = 0;
	int v, err;

	if ((err = vcf_priv(req)) != 0)
		return (err);
	sx_xlock(&vcf_lock);
	if (arg2 == -1) {
		err = emc_rd(EMC_TACH_HI, &hi);
		if (err == 0)
			err = emc_rd(EMC_TACH_LO, &lo);
		v = (hi << 5) | (lo >> 3);
	} else {
		err = emc_rd((uint8_t)arg2, &hi);
		v = hi;
	}
	sx_xunlock(&vcf_lock);
	if (err != 0)
		return (err);
	return (sysctl_handle_int(oidp, &v, 0, req));
}

/*
 * Apply the PWM profile after the recipe (lock held). Only 0x2b bits 1:0
 * (read-modify-write, PWM2's bits kept), 0x41 and 0x2a bit 0 are written.
 */
static int
vcf_pwm_profile_locked(void)
{
	uint8_t v;
	int err = 0;

	if (vcf_pwm_base >= 0) {
		if ((err = vcf_rd2(EMC_ADDR, EMC_PWM_BASE, &v)) != 0)
			return (err);
		v = (v & ~0x03) | (vcf_pwm_base & 0x03);
		if ((err = emc_wr(EMC_PWM_BASE, v)) != 0)
			return (err);
	}
	if (vcf_pwm_divide > 0 && (err = emc_wr(EMC_PWM1_DIVIDE, vcf_pwm_divide)) != 0)
		return (err);
	if (vcf_pwm_polarity >= 0) {
		if ((err = vcf_rd2(EMC_ADDR, EMC_PWM_CONFIG, &v)) != 0)
			return (err);
		v = (v & ~0x01) | (vcf_pwm_polarity & 0x01);
		err = emc_wr(EMC_PWM_CONFIG, v);
	}
	return (err);
}

static int
sysctl_pwm(SYSCTL_HANDLER_ARGS)
{
	int *p = arg1, v = *p, err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	if ((p == &vcf_pwm_base && (v < -1 || v > 3)) ||
	    (p == &vcf_pwm_divide && (v < 0 || v > 255)) ||
	    (p == &vcf_pwm_polarity && (v < -1 || v > 1)))
		return (EINVAL);
	if (p == &vcf_pwm_polarity && v == 1 && vcf_board == VCF_BOARD_520B)
		return (EPERM);		/* inverted drive would stop the fan at "full" */
	sx_xlock(&vcf_lock);
	*p = v;			/* takes effect at the next configure */
	sx_xunlock(&vcf_lock);
	return (0);
}

static int
sysctl_configure(SYSCTL_HANDLER_ARGS)
{
	int v = 0, err, i;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	if (v != 1)
		return (EINVAL);
	sx_xlock(&vcf_lock);
	if (vcf_unloading)
		err = EBUSY;
	else if (!vcf_identified)
		err = EPERM;
	for (i = 0; err == 0 && i < (int)nitems(emc_recipe); i++)
		err = emc_wr(emc_recipe[i].reg, emc_recipe[i].val);
	if (err == 0)
		err = vcf_pwm_profile_locked();
	if (err == 0 && pins_allowed())	/* the only place 0x1c is set */
		err = pca1c_apply_locked();
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_wd(SYSCTL_HANDLER_ARGS)
{
	int v = vcf_wd_s, err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	if (v != 0 && (v < WD_MIN_S || v > WD_MAX_S))
		return (EINVAL);
	sx_xlock(&vcf_lock);
	if (vcf_unloading)
		err = EBUSY;
	else if (v != 0 && !vcf_identified)
		err = EPERM;		/* nothing to guard; would only fail-loop */
	else if (v == 0 && vcf_wd_s != 0 && vcf_identified &&
	    (err = vcf_full_locked("watchdog disarm")) != 0)
		err = EIO;		/* stay armed; the fan may be slow */
	else {
		vcf_wd_s = v;
		if (v != 0)
			vcf_pet_locked();
	}
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_pet(SYSCTL_HANDLER_ARGS)
{
	int v = 0, err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	sx_xlock(&vcf_lock);
	if (vcf_wd_s == 0)
		err = EPERM;		/* a live loop must learn it is disarmed */
	else
		vcf_pet_locked();
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_pins(SYSCTL_HANDLER_ARGS)
{
	int v = pins_allowed(), err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	sx_xlock(&vcf_lock);
	vcf_pins_enable = v ? 1 : 0;
	sx_xunlock(&vcf_lock);
	return (0);
}

static int
sysctl_pins_release(SYSCTL_HANDLER_ARGS)
{
	int v = 0, err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	if (v != 1)
		return (EINVAL);
	sx_xlock(&vcf_lock);
	err = pca1c_release_locked();
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_pins_failsafe(SYSCTL_HANDLER_ARGS)
{
	int v = 0, err;

	err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if ((err = vcf_priv(req)) != 0)
		return (err);
	if (v != 1)
		return (EINVAL);
	sx_xlock(&vcf_lock);
	err = vcf_failsafe_pins_locked("requested");
	sx_xunlock(&vcf_lock);
	return (err);
}

static int
sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	static const uint8_t regs[] = { 0xfd, 0xfe, 0xff, 0x40, 0x41, 0x42, 0x46,
	    0x2a, 0x2b, 0xe2, 0x50, 0x4e, 0x4f, 0x00, 0x02, 0x04, 0x06 };
	struct sbuf sb;
	uint8_t v, pr[4] = { 0, 0, 0, 0 };
	int i, err, ok[4] = { 0, 0, 0, 0 };

	if ((err = vcf_priv(req)) != 0)
		return (err);
	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	sx_xlock(&vcf_lock);
	sbuf_printf(&sb, "emc2104@0x2f:");
	for (i = 0; i < (int)nitems(regs); i++) {
		if (emc_rd(regs[i], &v) == 0)
			sbuf_printf(&sb, " %02x=%02x", regs[i], v);
		else
			sbuf_printf(&sb, " %02x=NAK", regs[i]);
	}
	sbuf_printf(&sb, " pca9557@0x1c:");
	for (i = PCA_IN; i <= PCA_CFG; i++) {
		if (vcf_xfer(PCA_FAN_ADDR, 1, i, &v) == 0) {
			sbuf_printf(&sb, " %d=%02x", i, v);
			pr[i] = v;
			ok[i] = 1;
		} else
			sbuf_printf(&sb, " %d=NAK", i);
	}
	if (ok[PCA_IN] && ok[PCA_POL])
		sbuf_printf(&sb, " lvl=%02x", pr[PCA_IN] ^ pr[PCA_POL]);
	sbuf_printf(&sb, " pins_saved=%d", vcf_pins_saved);
	sx_xunlock(&vcf_lock);
	err = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (err);
}

static void
vcf_sysctl_init(void)
{
	struct sysctl_oid *node;
	struct sysctl_oid_list *l;

	sysctl_ctx_init(&vcf_ctx);
	node = SYSCTL_ADD_NODE(&vcf_ctx, SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO,
	    "vc_fan", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "VeloCloud 5x0 EMC2104 fan");
	l = SYSCTL_CHILDREN(node);
	SYSCTL_ADD_STRING(&vcf_ctx, l, OID_AUTO, "board", CTLFLAG_RD,
	    __DECONST(char *, vcf_board == VCF_BOARD_520B ? "edge520b" : "edge540b"), 0,
	    "board profile");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "identified", CTLFLAG_RD,
	    &vcf_identified, 0, "EMC2104 product and manufacturer ID read at load");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "chip_id", CTLFLAG_RD,
	    &vcf_chip_id, 0, "0xfd at 0x2f as read at load (-1 = no answer)");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "wd_fired", CTLFLAG_RD,
	    &vcf_wd_fired, 0, "watchdog fail-safe events since load");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "wd_failing", CTLFLAG_RD,
	    __DEVOLATILE(int *, &vcf_wd_failing), 0,
	    "1 while a watchdog full write has not verified");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "wd_retries", CTLFLAG_RD,
	    &vcf_wd_retries, 0, "retries of a failing watchdog full write");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "bus_stuck", CTLFLAG_RD,
	    &vcf_bus_stuck, 0, "transfers refused because SDA stayed low");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "duty",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, sysctl_duty, "I",
	    "EMC2104 fan setting 0x40 (0..255)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "tach",
	    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, -1, sysctl_rdreg, "I",
	    "raw tach count, 0x1fff = stopped");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "spinup_cfg",
	    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, EMC_SPINUP_CFG,
	    sysctl_rdreg, "I", "EMC2104 0x46");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "configure",
	    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, sysctl_configure, "I",
	    "write 1: open-loop PWM recipe + fan pins (if pins_enable)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "watchdog_s",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, sysctl_wd, "I",
	    "0 = disarmed (forces verified full); 5..120 s");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pet",
	    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, sysctl_pet, "I",
	    "write 1: restart the watchdog period (EPERM when disarmed)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pins_enable",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0, sysctl_pins, "I",
	    "may the 0x1c fan pins (bits 6/7) be written at configure");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pwm_base",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, &vcf_pwm_base, 0, sysctl_pwm, "I",
	    "-1 recipe; PWM_BASE1 0=26k 1=19.5k 2=4.88k 3=2.44k Hz (at configure)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pwm_divide",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, &vcf_pwm_divide, 0, sysctl_pwm, "I",
	    "0 recipe; 1..255 PWM1 divide 0x41 (at configure)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pwm_polarity",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, &vcf_pwm_polarity, 0, sysctl_pwm, "I",
	    "-1 recipe; 0x2a bit 0 POLARITY1 (at configure)");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pins_release",
	    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, sysctl_pins_release, "I",
	    "write 1: restore 0x1c bits 6/7 (CFG, then OUT) to their pre-configure values");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "pins_failsafe",
	    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, sysctl_pins_failsafe, "I",
	    "write 1: edge520b: CFG bit 7 = 1 (input), fan full by hardware");
	SYSCTL_ADD_INT(&vcf_ctx, l, OID_AUTO, "pins_rel_failing", CTLFLAG_RD,
	    &vcf_rel_failing, 0, "1 while an edge520b fail-safe release has not succeeded");
	SYSCTL_ADD_PROC(&vcf_ctx, l, OID_AUTO, "dump",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE | CTLFLAG_SKIP, NULL, 0,
	    sysctl_dump, "A", "read-only register dump");
}

/* ---------------------------------------------------------------- lifecycle */

static void
vcf_shutdown(void *arg, int howto)
{
	const char *why = arg;
	int nolock = SCHEDULER_STOPPED() || kdb_active;

	/*
	 * In a panic or from the debugger do not take the lock: its owner
	 * may be a stopped thread. Recover the bus instead, since another CPU may
	 * have stopped mid-transfer.
	 */
	if (!nolock)
		sx_xlock(&vcf_lock);
	vcf_unloading = 1;		/* no later sub-full write is accepted */
	if (nolock)
		vcf_bus_recover();
	(void)vcf_failsafe_pins_locked(why);	/* the hardware path first */
	(void)vcf_full_locked(why);
	if (!nolock)
		sx_xunlock(&vcf_lock);
}

/* SMBIOS planar product EDGE520/EDGE540 with version 2.x = rev B (read
 * here, not from vc_board, so this module never depends on vc_board). */
static enum vcf_board
vcf_board_detect(void)
{
	char *prod, *ver;
	enum vcf_board b = VCF_BOARD_NONE;

	prod = kern_getenv("smbios.planar.product");
	ver = kern_getenv("smbios.planar.version");
	if (prod != NULL && ver != NULL && ver[0] == '2') {
		if (strcmp(prod, "EDGE520") == 0)
			b = VCF_BOARD_520B;
		else if (strcmp(prod, "EDGE540") == 0)
			b = VCF_BOARD_540B;
	}
	printf("vc_fan: SMBIOS planar \"%s\" version \"%s\"\n",
	    prod ? prod : "(none)", ver ? ver : "(none)");
	if (prod != NULL)
		freeenv(prod);
	if (ver != NULL)
		freeenv(ver);
	return (b);
}

/* Has vc_board already run on this boot? Its sysctl exists only then. */
static int
vcf_board_module_loaded(void)
{
	char name[] = "hw.velocloud.board_name";
	char buf[32];
	size_t len = sizeof(buf);

	return (kernel_sysctlbyname(curthread, name, buf, &len, NULL, 0, NULL, 0) == 0);
}

static int
vcf_load(void)
{
	uint8_t id = 0, mfr = 0, c1 = 0, lut = 0;

	vcf_board = vcf_board_detect();
	if (vcf_board == VCF_BOARD_NONE) {
		printf("vc_fan: not a VeloCloud Edge 520/540 rev B; not loading\n");
		return (ENXIO);
	}
	if (vcf_board == VCF_BOARD_520B) {
		/* edge520b profile (measured): 2441 Hz / 64 = 38.1 Hz; duty
		 * is used only as 0 or 255 by the daemon. Tunables override. */
		if (vcf_pwm_base < 0)
			vcf_pwm_base = 3;
		if (vcf_pwm_divide == 0)
			vcf_pwm_divide = 64;
	}
	if (!vcf_board_module_loaded()) {
		printf("vc_fan: vc_board has not run (hw.velocloud.board_name absent); "
		    "load vc_fan only from 06-velo-fan or 'velo5x0-fan load'\n");
		return (ENXIO);
	}
	sx_init(&vcf_lock, "vc_fan");
	vcf_tq = taskqueue_create("vc_fan", M_WAITOK, taskqueue_thread_enqueue, &vcf_tq);
	taskqueue_start_threads(&vcf_tq, 1, PWAIT, "vc_fan watchdog");
	TASK_INIT(&vcf_wd_task, 0, vcf_wd_task_fn, NULL);
	callout_init(&vcf_wd_callout, 1);

	sx_xlock(&vcf_lock);
	/* bus idle: SCL high, SDA released, recovered if a slave holds it */
	vc_gpio_pin_setflags(VCF_PIN_SCL, VC_GPIO_OUTPUT);
	vc_gpio_pin_set(VCF_PIN_SCL, 1);
	vcf_bus_recover();
	/* clear any bit-7 drive left by a reset without a shutdown
	 * path, at the earliest point this module controls (before identify) */
	(void)vcf_failsafe_pins_locked("module load");
	if (vcf_rd2(EMC_ADDR, EMC_PRODUCT_ID_REG, &id) != 0) {
		vcf_chip_id = -1;
		printf("vc_fan: no stable answer at 0x2f; writing nothing\n");
	} else {
		vcf_chip_id = id;
		if (vcf_rd2(EMC_ADDR, EMC_MFR_ID_REG, &mfr) != 0)
			mfr = 0;
		vcf_identified = (id == EMC_PRODUCT_ID && mfr == EMC_MFR_ID);
		printf("vc_fan: 0x2f product ID 0x%02x manufacturer 0x%02x%s\n", id, mfr,
		    vcf_identified ? " (EMC2104)" : ": not an EMC2104, writing nothing");
	}
	if (vcf_identified) {
		/* report a closed-loop or LUT mode in
		 * which 0x40 would not hold 255; configure clears it */
		if (emc_rd(EMC_FAN_CONFIG1, &c1) == 0 && emc_rd(EMC_LUT_CONFIG, &lut) == 0 &&
		    ((c1 & 0x80) || (lut & 0x20)))
			printf("vc_fan: WARNING fan config 0x42=0x%02x 0x50=0x%02x: RPM "
			    "or LUT mode active until configure\n", c1, lut);
		(void)vcf_full_locked("module load");	/* F6 */
	}
	sx_xunlock(&vcf_lock);

	vcf_sysctl_init();
	/* pre_sync runs before any crash dump; final is the last chance */
	vcf_tag_presync = EVENTHANDLER_REGISTER(shutdown_pre_sync, vcf_shutdown,
	    __DECONST(void *, "shutdown"), SHUTDOWN_PRI_FIRST);
	vcf_tag_final = EVENTHANDLER_REGISTER(shutdown_final, vcf_shutdown,
	    __DECONST(void *, "shutdown final"), SHUTDOWN_PRI_FIRST);
	callout_reset(&vcf_wd_callout, hz, vcf_wd_tick, NULL);
	vcf_inited = 1;		/* only now does unload have anything to undo */
	return (0);		/* loads even if not identified: sysctls report why */
}

static int
vcf_unload(void)
{
	int err;

	/*
	 * The kernel calls MOD_UNLOAD right after a MOD_LOAD that returned an
	 * error. A refused load initialised nothing (no lock, no taskqueue, no
	 * callout, no sysctls), so there is nothing to undo.
	 */
	if (!vcf_inited)
		return (0);
	/* block writes, then full verified first, or refuse to unload */
	sx_xlock(&vcf_lock);
	vcf_unloading = 1;
	(void)vcf_failsafe_pins_locked("module unload");	/* even if refused */
	if (vcf_identified && (err = vcf_full_locked("module unload")) != 0) {
		vcf_unloading = 0;
		sx_xunlock(&vcf_lock);
		printf("vc_fan: unload refused: full duty not verified\n");
		return (EBUSY);
	}
	sx_xunlock(&vcf_lock);
	/* then no sysctl handler can be running or start after this */
	if (sysctl_ctx_free(&vcf_ctx) != 0) {
		vcf_unloading = 0;
		return (EBUSY);
	}
	callout_drain(&vcf_wd_callout);
	taskqueue_free(vcf_tq);		/* drains the task */
	EVENTHANDLER_DEREGISTER(shutdown_pre_sync, vcf_tag_presync);
	EVENTHANDLER_DEREGISTER(shutdown_final, vcf_tag_final);
	vcf_identified = 0;
	vcf_inited = 0;
	sx_destroy(&vcf_lock);
	return (0);
}

static int
vcf_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
		return (vcf_load());
	case MOD_UNLOAD:
		return (vcf_unload());
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t vcf_mod = { "vc_fan", vcf_modevent, NULL };
DECLARE_MODULE(vc_fan, vcf_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_DEPEND(vc_fan, vc_pcu_gpio, 1, 1, 1);	/* NOT vc_board */
MODULE_VERSION(vc_fan, 6);
