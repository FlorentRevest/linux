/*
 * Copyright (c) 2016 Maxime Ripard. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/of_address.h>

#include "ccu_common.h"
#include "ccu_reset.h"

#include "ccu_div.h"
#include "ccu_gate.h"
#include "ccu_mp.h"
#include "ccu_mult.h"
#include "ccu_nk.h"
#include "ccu_nkm.h"
#include "ccu_nkmp.h"
#include "ccu_nm.h"
#include "ccu_phase.h"

#include "ccu-sun5i-a13.h"

static SUNXI_CCU_GATE(ve_clk, "ve", "pll4",
		      0x13c, BIT(31), CLK_SET_RATE_PARENT);

static SUNXI_CCU_GATE(avs_clk,	"avs",	"osc24M",
		      0x144, BIT(31), 0);

static struct ccu_common *sun5i_a13_ccu_clks[] = {
	&ve_clk.common,
	&avs_clk.common,
};

static struct clk_hw_onecell_data sun5i_a13_hw_clks = {
	.hws	= {
		[CLK_VE]		= &ve_clk.common.hw,
		[CLK_AVS]		= &avs_clk.common.hw,
	},
	.num	= CLK_NUMBER,
};

static struct ccu_reset_map sun5i_a13_ccu_resets[] = {
	[RST_VE]		=  { 0x13c, BIT(0) },
};

static const struct sunxi_ccu_desc sun5i_a13_ccu_desc = {
	.ccu_clks	= sun5i_a13_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun5i_a13_ccu_clks),

	.hw_clks	= &sun5i_a13_hw_clks,

	.resets		= sun5i_a13_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun5i_a13_ccu_resets),
};

static void __init sun5i_a13_ccu_setup(struct device_node *node)
{
	void __iomem *reg;

	reg = of_iomap(node, 0);
	if (IS_ERR(reg)) {
		pr_err("%s: Could not map the clock registers\n",
		       of_node_full_name(node));
		return;
	}

	sunxi_ccu_probe(node, reg, &sun5i_a13_ccu_desc);
}

CLK_OF_DECLARE(sun5i_A13_ccu, "allwinner,sun5i-a13-ccu",
	       sun5i_a13_ccu_setup);
