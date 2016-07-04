/*
 * Copyright 2016 Maxime Ripard
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CCU_SUN5I_A13_H_
#define _CCU_SUN5I_A13_H_

#include <dt-bindings/clock/sun5i-a13-ccu.h>
#include <dt-bindings/reset/sun5i-a13-ccu.h>

#define CLK_PLL_VIDEO		6
#define CLK_PLL_VE		7
#define CLK_PLL_DDR		8
#define CLK_PLL_PERIPH0_2X	8

#define CLK_AHB1		16

/* All the bus gates are exported */

/* The first bunch of module clocks are exported */

#define CLK_DRAM		96

/* All the DRAM gates are exported */

/* Some more module clocks are exported */

/* And the GPU module clock is exported */

#define CLK_NUMBER		2

#endif /* _CCU_SUN5I_A13_H_ */
