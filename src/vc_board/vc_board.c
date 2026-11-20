/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 VeloCloud/VMware
 *
 * VeloCloud Edge board identification and hardware reset control.
 *
 * 1. Board identification via SMBIOS kenv
 * 2. PCA9557 I2C GPIO expander via bitbang I2C (SC8=CLK, SC9=DATA)
 * 3. Hardware reset release for Marvell 88E6176 switches and 88E1514 PHYs
 *
 * PCA9557 at I2C 7-bit addr 0x18:
 *   Pin 0: RST_PCIE_0, Pin 1: RST_PCIE_1
 *   Pin 2: RST_SW_A, Pin 3: RST_SW_B, Pin 4: RST_1514
 *   Pin 5: RFKILL_0, Pin 6: RFKILL_1, Pin 7: USB_PWREN
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "vc_pcu_gpio.h"
#include "vc_board.h"

/* I2C bitbang pins (SC well) — PCA9557 is on SMB1 bus
 * From Linux: EDGE500_GPIO_SMB1_CLK = SC12, EDGE500_GPIO_SMB1_DATA = SC11
 */
#define	VC_I2C_PIN_CLK		12	/* SC12: SMB1_CLK */
#define	VC_I2C_PIN_DATA		11	/* SC11: SMB1_DATA */
#define	I2C_HALF_PERIOD_US	5

/* PCA9557 */
#define	PCA9557_ADDR		0x18	/* 0x30 >> 1 */
#define	PCA9557_REG_IN		0x00
#define	PCA9557_REG_OUT		0x01
#define	PCA9557_REG_POL		0x02
#define	PCA9557_REG_CFG		0x03
#define	PCA9557_INIT_OUT	0xe3	/* sw/phy in reset, rest de-asserted */
#define	PCA9557_RELEASED_OUT	0xff	/* all de-asserted */

/* ---- Board matching ---- */

struct vc_board_match {
	const char		*board_name;
	const char		*board_ver;
	enum vc_board_id	id;
	const char		*ident;
};

static const struct vc_board_match vc_board_table[] = {
	{ "EDGE520",	"2",	VC_BOARD_EDGE520B,	"edge520b" },
	{ "EDGE540",	"2",	VC_BOARD_EDGE540B,	"edge540b" },
	{ "EDGE520",	NULL,	VC_BOARD_EDGE520,	"edge520" },
	{ "EDGE540",	NULL,	VC_BOARD_EDGE540,	"edge540" },
	{ "EDGE500",	NULL,	VC_BOARD_EDGE500,	"edge500" },
	{ "EDGE510",	NULL,	VC_BOARD_EDGE510,	"edge510" },
	{ "RCC-VE",	NULL,	VC_BOARD_EDGE510,	"edge510" },
	{ NULL,		NULL,	VC_BOARD_NONE,		"unknown" },
};

static enum vc_board_id	vc_detected_id = VC_BOARD_NONE;
static const char	*vc_detected_name = "unknown";

/* Exported API */
enum vc_board_id
vc_board_get_id(void)
{
	return (vc_detected_id);
}

const char *
vc_board_get_name(void)
{
	return (vc_detected_name);
}

/* ---- sysctl ---- */

SYSCTL_NODE(_hw, OID_AUTO, velocloud, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "VeloCloud board information");

static int
sysctl_vc_board_id(SYSCTL_HANDLER_ARGS)
{
	int val = (int)vc_detected_id;
	return (sysctl_handle_int(oidp, &val, 0, req));
}

SYSCTL_PROC(_hw_velocloud, OID_AUTO, board_id,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
    NULL, 0, sysctl_vc_board_id, "I",
    "VeloCloud board ID");

static int
sysctl_vc_board_name(SYSCTL_HANDLER_ARGS)
{
	return (sysctl_handle_string(oidp, __DECONST(char *, vc_detected_name),
	    0, req));
}

SYSCTL_PROC(_hw_velocloud, OID_AUTO, board_name,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
    NULL, 0, sysctl_vc_board_name, "A",
    "VeloCloud board name");

/* ================================================================
 * I2C Bitbang — uses vc_gpio_pin_* from vc_pcu_gpio
 * ================================================================ */

static __inline void
i2c_scl_set(int val)
{
	vc_gpio_pin_set(VC_I2C_PIN_CLK, val);
}

static __inline void
i2c_sda_set(int val)
{
	vc_gpio_pin_set(VC_I2C_PIN_DATA, val);
}

static __inline int
i2c_sda_get(void)
{
	unsigned int val;
	vc_gpio_pin_get(VC_I2C_PIN_DATA, &val);
	return (val != 0);
}

static __inline void
i2c_sda_dir_out(void)
{
	vc_gpio_pin_setflags(VC_I2C_PIN_DATA, VC_GPIO_OUTPUT);
}

static __inline void
i2c_sda_dir_in(void)
{
	vc_gpio_pin_setflags(VC_I2C_PIN_DATA, VC_GPIO_INPUT);
}

static void
i2c_start(void)
{
	i2c_sda_dir_out();
	i2c_sda_set(1);
	i2c_scl_set(1);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_sda_set(0);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_scl_set(0);
	DELAY(I2C_HALF_PERIOD_US);
}

static void
i2c_stop(void)
{
	i2c_sda_dir_out();
	i2c_sda_set(0);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_scl_set(1);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_sda_set(1);
	DELAY(I2C_HALF_PERIOD_US);
}

static int
i2c_send_byte(uint8_t byte)
{
	int i, ack;

	i2c_sda_dir_out();
	for (i = 7; i >= 0; i--) {
		i2c_sda_set((byte >> i) & 1);
		DELAY(I2C_HALF_PERIOD_US);
		i2c_scl_set(1);
		DELAY(I2C_HALF_PERIOD_US);
		i2c_scl_set(0);
	}

	i2c_sda_dir_in();
	DELAY(I2C_HALF_PERIOD_US);
	i2c_scl_set(1);
	DELAY(I2C_HALF_PERIOD_US);
	ack = i2c_sda_get();
	i2c_scl_set(0);
	DELAY(I2C_HALF_PERIOD_US);

	return (ack);
}

static uint8_t
i2c_recv_byte(int send_ack)
{
	int i;
	uint8_t byte = 0;

	i2c_sda_dir_in();
	for (i = 7; i >= 0; i--) {
		DELAY(I2C_HALF_PERIOD_US);
		i2c_scl_set(1);
		DELAY(I2C_HALF_PERIOD_US);
		if (i2c_sda_get())
			byte |= (1u << i);
		i2c_scl_set(0);
	}

	i2c_sda_dir_out();
	i2c_sda_set(send_ack ? 0 : 1);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_scl_set(1);
	DELAY(I2C_HALF_PERIOD_US);
	i2c_scl_set(0);
	DELAY(I2C_HALF_PERIOD_US);

	return (byte);
}

/* ---- PCA9557 I2C ---- */

static int
pca9557_write(uint8_t reg, uint8_t val)
{
	int err = 0;

	i2c_start();
	if (i2c_send_byte((PCA9557_ADDR << 1) | 0)) {
		printf("vc_board: PCA9557 addr NACK (write)\n");
		err = -1;
		goto out;
	}
	if (i2c_send_byte(reg)) {
		printf("vc_board: PCA9557 reg NACK\n");
		err = -1;
		goto out;
	}
	if (i2c_send_byte(val)) {
		printf("vc_board: PCA9557 data NACK\n");
		err = -1;
		goto out;
	}
out:
	i2c_stop();
	return (err);
}

static int
pca9557_read(uint8_t reg)
{
	int val;

	i2c_start();
	if (i2c_send_byte((PCA9557_ADDR << 1) | 0)) {
		printf("vc_board: PCA9557 addr NACK (read-wr)\n");
		i2c_stop();
		return (-1);
	}
	if (i2c_send_byte(reg)) {
		printf("vc_board: PCA9557 reg NACK (read)\n");
		i2c_stop();
		return (-1);
	}

	i2c_start();
	if (i2c_send_byte((PCA9557_ADDR << 1) | 1)) {
		printf("vc_board: PCA9557 addr NACK (read-rd)\n");
		i2c_stop();
		return (-1);
	}
	val = i2c_recv_byte(0);
	i2c_stop();

	return (val);
}

/* ---- Board detection ---- */

static void
vc_board_detect(void)
{
	char *board_name, *board_ver;
	const struct vc_board_match *m;

	board_name = kern_getenv("smbios.planar.product");
	board_ver = kern_getenv("smbios.planar.version");

	printf("vc_board: SMBIOS planar.product=\"%s\" version=\"%s\"\n",
	    board_name ? board_name : "(null)",
	    board_ver ? board_ver : "(null)");

	if (board_name == NULL) {
		printf("vc_board: no SMBIOS board name available\n");
		goto out;
	}

	for (m = vc_board_table; m->board_name != NULL; m++) {
		if (strcmp(board_name, m->board_name) != 0)
			continue;
		if (m->board_ver != NULL) {
			if (board_ver == NULL)
				continue;
			if (strncmp(board_ver, m->board_ver,
			    strlen(m->board_ver)) != 0)
				continue;
		}
		vc_detected_id = m->id;
		vc_detected_name = m->ident;
		printf("vc_board: detected %s (id=%d)\n",
		    m->ident, (int)m->id);
		goto out;
	}

	printf("vc_board: unknown board \"%s\"\n", board_name);

out:
	if (board_name != NULL)
		freeenv(board_name);
	if (board_ver != NULL)
		freeenv(board_ver);
}

/* ---- Reset control ---- */

static int
vc_board_reset_init(void)
{
	int val;

	printf("vc_board: initializing PCA9557 (I2C 0x%02x)\n", PCA9557_ADDR);

	/* Configure I2C pins */
	vc_gpio_pin_setflags(VC_I2C_PIN_CLK, VC_GPIO_OUTPUT);
	vc_gpio_pin_set(VC_I2C_PIN_CLK, 1);
	vc_gpio_pin_setflags(VC_I2C_PIN_DATA, VC_GPIO_OUTPUT);
	vc_gpio_pin_set(VC_I2C_PIN_DATA, 1);
	DELAY(100);

	/* Test read */
	val = pca9557_read(PCA9557_REG_IN);
	if (val < 0) {
		printf("vc_board: PCA9557 not responding\n");
		return (-1);
	}
	printf("vc_board: PCA9557 IN=0x%02x\n", val);

	/* Disable polarity inversion */
	if (pca9557_write(PCA9557_REG_POL, 0x00) < 0)
		return (-1);

	/* Set output — resets asserted */
	if (pca9557_write(PCA9557_REG_OUT, PCA9557_INIT_OUT) < 0)
		return (-1);

	/* All pins as outputs */
	if (pca9557_write(PCA9557_REG_CFG, 0x00) < 0)
		return (-1);

	DELAY(10000);

	val = pca9557_read(PCA9557_REG_OUT);
	printf("vc_board: PCA9557 OUT=0x%02x (expected 0x%02x)\n",
	    val, PCA9557_INIT_OUT);

	/* Release resets */
	printf("vc_board: releasing switch and PHY resets\n");
	if (pca9557_write(PCA9557_REG_OUT, PCA9557_RELEASED_OUT) < 0)
		return (-1);

	DELAY(50000);

	val = pca9557_read(PCA9557_REG_OUT);
	printf("vc_board: PCA9557 OUT=0x%02x (released)\n", val);
	printf("vc_board: hardware resets released\n");
	return (0);
}

/* ---- Module lifecycle ---- */

static int
vc_board_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
		vc_board_detect();

		if (vc_detected_id >= VC_BOARD_EDGE500 &&
		    vc_detected_id <= VC_BOARD_EDGE540B) {
			vc_board_reset_init();
		}
		return (0);

	case MOD_UNLOAD:
		vc_detected_id = VC_BOARD_NONE;
		vc_detected_name = "unknown";
		printf("vc_board: unloaded\n");
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t vc_board_mod = {
	"vc_board",
	vc_board_modevent,
	NULL
};

DECLARE_MODULE(vc_board, vc_board_mod, SI_SUB_DRIVERS, SI_ORDER_SECOND);
MODULE_DEPEND(vc_board, vc_pcu_gpio, 1, 1, 1);
MODULE_VERSION(vc_board, 1);
