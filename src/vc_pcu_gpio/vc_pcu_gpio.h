/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared header for vc_pcu_gpio — Intel Atom C2000 PCU GPIO.
 * Provides direct pin access for other VeloCloud kmods (vc_board, vc_mdio).
 */

#ifndef _VC_PCU_GPIO_H_
#define _VC_PCU_GPIO_H_

#define	VC_GPIO_INPUT	1
#define	VC_GPIO_OUTPUT	0

int vc_gpio_pin_get(uint32_t pin, unsigned int *val);
int vc_gpio_pin_set(uint32_t pin, unsigned int val);
int vc_gpio_pin_setflags(uint32_t pin, int dir); /* VC_GPIO_INPUT/OUTPUT */

#endif /* _VC_PCU_GPIO_H_ */
