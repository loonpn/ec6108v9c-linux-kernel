// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hi3798 Clock and Reset Generator Driver
 *
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 */

#include <dt-bindings/clock/histb-clock.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "clk.h"
#include "crg.h"

/* hi3798 core CRG */
#define HI3798_INNER_CLK_OFFSET	128
#define HI3798_FIXED_24M	(HI3798_INNER_CLK_OFFSET + 00)
#define HI3798_FIXED_25M	(HI3798_INNER_CLK_OFFSET + 01)
#define HI3798_FIXED_50M	(HI3798_INNER_CLK_OFFSET + 02)
#define HI3798_FIXED_75M	(HI3798_INNER_CLK_OFFSET + 03)
#define HI3798_FIXED_100M	(HI3798_INNER_CLK_OFFSET + 04)
#define HI3798_FIXED_150M	(HI3798_INNER_CLK_OFFSET + 05)
#define HI3798_FIXED_200M	(HI3798_INNER_CLK_OFFSET + 06)
#define HI3798_FIXED_250M	(HI3798_INNER_CLK_OFFSET + 07)
#define HI3798_FIXED_300M	(HI3798_INNER_CLK_OFFSET + 08)
#define HI3798_FIXED_400M	(HI3798_INNER_CLK_OFFSET + 09)
#define HI3798_MMC_MUX		(HI3798_INNER_CLK_OFFSET + 10)
#define HI3798_ETH_PUB_CLK	(HI3798_INNER_CLK_OFFSET + 11)
#define HI3798_ETH_BUS_CLK	(HI3798_INNER_CLK_OFFSET + 12)
#define HI3798_ETH_BUS0_CLK	(HI3798_INNER_CLK_OFFSET + 13)
#define HI3798_ETH_BUS1_CLK	(HI3798_INNER_CLK_OFFSET + 14)
#define HI3798_COMBPHY1_MUX	(HI3798_INNER_CLK_OFFSET + 15)
#define HI3798_FIXED_12M	(HI3798_INNER_CLK_OFFSET + 16)
#define HI3798_FIXED_48M	(HI3798_INNER_CLK_OFFSET + 17)
#define HI3798_FIXED_60M	(HI3798_INNER_CLK_OFFSET + 18)
#define HI3798_FIXED_166P5M	(HI3798_INNER_CLK_OFFSET + 19)
#define HI3798_SDIO0_MUX	(HI3798_INNER_CLK_OFFSET + 20)
#define HI3798_COMBPHY0_MUX	(HI3798_INNER_CLK_OFFSET + 21)
#define HI3798_FIXED_3M		(HI3798_INNER_CLK_OFFSET + 22)
#define HI3798_FIXED_15M	(HI3798_INNER_CLK_OFFSET + 23)
#define HI3798_FIXED_83P3M	(HI3798_INNER_CLK_OFFSET + 24)

#define HI3798_CRG_NR_CLKS	256

#define HI3798_SYSCTRL_NR_CLKS	16
static const struct hisi_fixed_rate_clock hi3798_fixed_rate_clks[] = {
	{ HISTB_OSC_CLK, "clk_osc", NULL, 0, 24000000, },
	{ HISTB_APB_CLK, "clk_apb", NULL, 0, 100000000, },
	{ HISTB_AHB_CLK, "clk_ahb", NULL, 0, 200000000, },
	{ HI3798_FIXED_3M, "3m", NULL, 0, 3000000, },
	{ HI3798_FIXED_12M, "12m", NULL, 0, 12000000, },
	{ HI3798_FIXED_15M, "15m", NULL, 0, 15000000, },
	{ HI3798_FIXED_24M, "24m", NULL, 0, 24000000, },
	{ HI3798_FIXED_25M, "25m", NULL, 0, 25000000, },
	{ HI3798_FIXED_48M, "48m", NULL, 0, 48000000, },
	{ HI3798_FIXED_50M, "50m", NULL, 0, 50000000, },
	{ HI3798_FIXED_60M, "60m", NULL, 0, 60000000, },
	{ HI3798_FIXED_75M, "75m", NULL, 0, 75000000, },
	{ HI3798_FIXED_83P3M, "83p3m", NULL, 0, 83333333, },
	{ HI3798_FIXED_100M, "100m", NULL, 0, 100000000, },
	{ HI3798_FIXED_150M, "150m", NULL, 0, 150000000, },
	{ HI3798_FIXED_166P5M, "166p5m", NULL, 0, 165000000, },
	{ HI3798_FIXED_200M, "200m", NULL, 0, 200000000, },
	{ HI3798_FIXED_250M, "250m", NULL, 0, 250000000, },
};

struct hi3798_complex_clock {
	unsigned int	id;
	const char	*name;
	const char	*parent_name;
	unsigned long	flags;
	unsigned long	offset;
	u32		mask;
	u32		value;
};

struct hi3798_clk_complex {
	struct clk_hw	hw;
	void __iomem	*reg;
	u32		mask;
	u32		value;
};

#define to_complex_clk(_hw) container_of(_hw, struct hi3798_clk_complex, hw)

static int hi3798_clk_complex_prepare(struct clk_hw *hw)
{
	struct hi3798_clk_complex *clk = to_complex_clk(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	val &= ~(clk->mask);
	val |= clk->value;
	writel_relaxed(val, clk->reg);

	return 0;
}

static void hi3798_clk_complex_unprepare(struct clk_hw *hw)
{
	struct hi3798_clk_complex *clk = to_complex_clk(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	val &= ~(clk->mask);
	writel_relaxed(val, clk->reg);
}

static int hi3798_clk_complex_is_prepared(struct clk_hw *hw)
{
	struct hi3798_clk_complex *clk = to_complex_clk(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	return (val & clk->mask) == clk->value;
}

static const struct clk_ops hi3798_clk_complex_ops = {
	.prepare = hi3798_clk_complex_prepare,
	.unprepare = hi3798_clk_complex_unprepare,
	.is_prepared = hi3798_clk_complex_is_prepared,
};

static int hi3798_clk_register_complex(struct device *dev,
				       const void *clocks, size_t num,
				       struct hisi_clock_data *data)
{
	const struct hi3798_complex_clock *clks = clocks;
	void __iomem *base = data->base;

	for (int i = 0; i < num; i++) {
		struct hi3798_clk_complex *p_clk;
		struct clk_init_data init;
		int ret;

		p_clk = devm_kzalloc(dev, sizeof(*p_clk), GFP_KERNEL);
		if (!p_clk)
			return -ENOMEM;

		init.name = clks[i].name;
		init.ops = &hi3798_clk_complex_ops;

		init.flags = 0;
		init.parent_names =
			(clks[i].parent_name ? &clks[i].parent_name : NULL);
		init.num_parents = (clks[i].parent_name ? 1 : 0);

		p_clk->reg = base + clks[i].offset;
		p_clk->mask = clks[i].mask;
		p_clk->value = clks[i].value;
		p_clk->hw.init = &init;

		ret = devm_clk_hw_register(dev, &p_clk->hw);
		if (ret) {
			pr_err("%s: failed to register clock %s\n",
			       __func__, clks[i].name);
			return ret;
		}

		data->clk_data->hws[clks[i].id] = &p_clk->hw;
	}

	return 0;
}

/* hi3798CV200 */

static const char *const hi3798cv200_mmc_mux_p[] = {
		"100m", "50m", "25m", "200m", "150m" };
static u32 hi3798cv200_mmc_mux_table[] = {0, 1, 2, 3, 6};

static const char *const hi3798cv200_comphy_mux_p[] = {
		"100m", "25m"};
static u32 hi3798cv200_comphy_mux_table[] = {2, 3};

static const char *const hi3798cv200_sdio_mux_p[] = {
		"100m", "50m", "150m", "166p5m" };
static u32 hi3798cv200_sdio_mux_table[] = {0, 1, 2, 3};

static struct hisi_mux_clock hi3798cv200_mux_clks[] = {
	{ HI3798_MMC_MUX, "mmc_mux", hi3798cv200_mmc_mux_p,
		ARRAY_SIZE(hi3798cv200_mmc_mux_p), CLK_SET_RATE_PARENT,
		0xa0, 8, 3, 0, hi3798cv200_mmc_mux_table, },
	{ HI3798_COMBPHY0_MUX, "combphy0_mux", hi3798cv200_comphy_mux_p,
		ARRAY_SIZE(hi3798cv200_comphy_mux_p), CLK_SET_RATE_PARENT,
		0x188, 2, 2, 0, hi3798cv200_comphy_mux_table, },
	{ HI3798_COMBPHY1_MUX, "combphy1_mux", hi3798cv200_comphy_mux_p,
		ARRAY_SIZE(hi3798cv200_comphy_mux_p), CLK_SET_RATE_PARENT,
		0x188, 10, 2, 0, hi3798cv200_comphy_mux_table, },
	{ HI3798_SDIO0_MUX, "sdio0_mux", hi3798cv200_sdio_mux_p,
		ARRAY_SIZE(hi3798cv200_sdio_mux_p), CLK_SET_RATE_PARENT,
		0x9c, 8, 2, 0, hi3798cv200_sdio_mux_table, },
};

static u32 mmc_phase_regvals[] = {0, 1, 2, 3, 4, 5, 6, 7};
static u32 mmc_phase_degrees[] = {0, 45, 90, 135, 180, 225, 270, 315};

static struct hisi_phase_clock hi3798cv200_phase_clks[] = {
	{ HISTB_MMC_SAMPLE_CLK, "mmc_sample", "clk_mmc_ciu",
		CLK_SET_RATE_PARENT, 0xa0, 12, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HISTB_MMC_DRV_CLK, "mmc_drive", "clk_mmc_ciu",
		CLK_SET_RATE_PARENT, 0xa0, 16, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
};

static const struct hisi_gate_clock hi3798cv200_gate_clks[] = {
	/* UART */
	{ HISTB_UART2_CLK, "clk_uart2", "75m",
		CLK_SET_RATE_PARENT, 0x68, 4, 0, },
	/* I2C */
	{ HISTB_I2C0_CLK, "clk_i2c0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 4, 0, },
	{ HISTB_I2C1_CLK, "clk_i2c1", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 8, 0, },
	{ HISTB_I2C2_CLK, "clk_i2c2", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 12, 0, },
	{ HISTB_I2C3_CLK, "clk_i2c3", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 16, 0, },
	{ HISTB_I2C4_CLK, "clk_i2c4", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 20, 0, },
	/* SPI */
	{ HISTB_SPI0_CLK, "clk_spi0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x70, 0, 0, },
	/* SDIO */
	{ HISTB_SDIO0_BIU_CLK, "clk_sdio0_biu", "200m",
		CLK_SET_RATE_PARENT, 0x9c, 0, 0, },
	{ HISTB_SDIO0_CIU_CLK, "clk_sdio0_ciu", "sdio0_mux",
		CLK_SET_RATE_PARENT, 0x9c, 1, 0, },
	/* EMMC */
	{ HISTB_MMC_BIU_CLK, "clk_mmc_biu", "200m",
		CLK_SET_RATE_PARENT, 0xa0, 0, 0, },
	{ HISTB_MMC_CIU_CLK, "clk_mmc_ciu", "mmc_mux",
		CLK_SET_RATE_PARENT, 0xa0, 1, 0, },
	/* PCIE*/
	{ HISTB_PCIE_BUS_CLK, "clk_pcie_bus", "200m",
		CLK_SET_RATE_PARENT, 0x18c, 0, 0, },
	{ HISTB_PCIE_SYS_CLK, "clk_pcie_sys", "100m",
		CLK_SET_RATE_PARENT, 0x18c, 1, 0, },
	{ HISTB_PCIE_PIPE_CLK, "clk_pcie_pipe", "250m",
		CLK_SET_RATE_PARENT, 0x18c, 2, 0, },
	{ HISTB_PCIE_AUX_CLK, "clk_pcie_aux", "24m",
		CLK_SET_RATE_PARENT, 0x18c, 3, 0, },
	/* Ethernet */
	{ HI3798_ETH_PUB_CLK, "clk_pub", NULL,
		CLK_SET_RATE_PARENT, 0xcc, 5, 0, },
	{ HI3798_ETH_BUS_CLK, "clk_bus", "clk_pub",
		CLK_SET_RATE_PARENT, 0xcc, 0, 0, },
	{ HI3798_ETH_BUS0_CLK, "clk_bus_m0", "clk_bus",
		CLK_SET_RATE_PARENT, 0xcc, 1, 0, },
	{ HI3798_ETH_BUS1_CLK, "clk_bus_m1", "clk_bus",
		CLK_SET_RATE_PARENT, 0xcc, 2, 0, },
	{ HISTB_ETH0_MAC_CLK, "clk_mac0", "clk_bus_m0",
		CLK_SET_RATE_PARENT, 0xcc, 3, 0, },
	{ HISTB_ETH0_MACIF_CLK, "clk_macif0", "clk_bus_m0",
		CLK_SET_RATE_PARENT, 0xcc, 24, 0, },
	{ HISTB_ETH1_MAC_CLK, "clk_mac1", "clk_bus_m1",
		CLK_SET_RATE_PARENT, 0xcc, 4, 0, },
	{ HISTB_ETH1_MACIF_CLK, "clk_macif1", "clk_bus_m1",
		CLK_SET_RATE_PARENT, 0xcc, 25, 0, },
	/* COMBPHY0 */
	{ HISTB_COMBPHY0_CLK, "clk_combphy0", "combphy0_mux",
		CLK_SET_RATE_PARENT, 0x188, 0, 0, },
	/* COMBPHY1 */
	{ HISTB_COMBPHY1_CLK, "clk_combphy1", "combphy1_mux",
		CLK_SET_RATE_PARENT, 0x188, 8, 0, },
	/* USB2 */
	{ HISTB_USB2_BUS_CLK, "clk_u2_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb8, 0, 0, },
	{ HISTB_USB2_PHY_CLK, "clk_u2_phy", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 4, 0, },
	{ HISTB_USB2_12M_CLK, "clk_u2_12m", "12m",
		CLK_SET_RATE_PARENT, 0xb8, 2, 0 },
	{ HISTB_USB2_48M_CLK, "clk_u2_48m", "48m",
		CLK_SET_RATE_PARENT, 0xb8, 1, 0 },
	{ HISTB_USB2_UTMI_CLK, "clk_u2_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 5, 0 },
	{ HISTB_USB2_OTG_UTMI_CLK, "clk_u2_otg_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 3, 0 },
	{ HISTB_USB2_PHY1_REF_CLK, "clk_u2_phy1_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 0, 0 },
	{ HISTB_USB2_PHY2_REF_CLK, "clk_u2_phy2_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 2, 0 },
	/* USB3 */
	{ HISTB_USB3_BUS_CLK, "clk_u3_bus", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 0, 0 },
	{ HISTB_USB3_UTMI_CLK, "clk_u3_utmi", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 4, 0 },
	{ HISTB_USB3_PIPE_CLK, "clk_u3_pipe", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 3, 0 },
	{ HISTB_USB3_SUSPEND_CLK, "clk_u3_suspend", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 2, 0 },
	{ HISTB_USB3_BUS_CLK1, "clk_u3_bus1", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 16, 0 },
	{ HISTB_USB3_UTMI_CLK1, "clk_u3_utmi1", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 20, 0 },
	{ HISTB_USB3_PIPE_CLK1, "clk_u3_pipe1", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 19, 0 },
	{ HISTB_USB3_SUSPEND_CLK1, "clk_u3_suspend1", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 18, 0 },
};

static const struct hisi_clocks hi3798cv200_crg_clks = {
	.nr = HI3798_CRG_NR_CLKS,
	.fixed_rate_clks = hi3798_fixed_rate_clks,
	.fixed_rate_clks_num = ARRAY_SIZE(hi3798_fixed_rate_clks),
	.mux_clks = hi3798cv200_mux_clks,
	.mux_clks_num = ARRAY_SIZE(hi3798cv200_mux_clks),
	.phase_clks = hi3798cv200_phase_clks,
	.phase_clks_num = ARRAY_SIZE(hi3798cv200_phase_clks),
	.gate_clks = hi3798cv200_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798cv200_gate_clks),
};

static const struct hisi_gate_clock hi3798cv200_sysctrl_gate_clks[] = {
	{ HISTB_IR_CLK, "clk_ir", "24m",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HISTB_TIMER01_CLK, "clk_timer01", "24m",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HISTB_UART0_CLK, "clk_uart0", "75m",
		CLK_SET_RATE_PARENT, 0x48, 10, 0, },
};

static const struct hisi_clocks hi3798cv200_sysctrl_clks = {
	.nr = HI3798_SYSCTRL_NR_CLKS,
	.gate_clks = hi3798cv200_sysctrl_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798cv200_sysctrl_gate_clks),
};

/* hi3798MV100 */

static const char *const hi3798mv100_mmc_mux_p[] = {
		"75m", "100m", "50m", "15m" };
static u32 hi3798mv100_mmc_mux_table[] = {0, 1, 2, 3};

static struct hisi_mux_clock hi3798mv100_mux_clks[] = {
	{ HI3798_MMC_MUX, "mmc_mux", hi3798mv100_mmc_mux_p,
		ARRAY_SIZE(hi3798mv100_mmc_mux_p), CLK_SET_RATE_PARENT,
		0xa0, 8, 2, 0, hi3798mv100_mmc_mux_table, },
	{ HI3798_SDIO0_MUX, "sdio0_mux", hi3798mv100_mmc_mux_p,
		ARRAY_SIZE(hi3798mv100_mmc_mux_p), CLK_SET_RATE_PARENT,
		0x9c, 8, 2, 0, hi3798mv100_mmc_mux_table, },
};

static const struct hisi_gate_clock hi3798mv100_gate_clks[] = {
	/* NAND */
	/* hi3798MV100 NAND driver does not get into mainline yet,
	 * expose these clocks when it gets ready */
	/* { HISTB_NAND_CLK, "clk_nand", "clk_apb",
		CLK_SET_RATE_PARENT, 0x60, 0, 0, }, */
	/* UART */
	{ HISTB_UART1_CLK, "clk_uart1", "3m",
		CLK_SET_RATE_PARENT | CLK_IS_CRITICAL, 0x68, 0, 0, },
	{ HISTB_UART2_CLK, "clk_uart2", "83p3m",
		CLK_SET_RATE_PARENT, 0x68, 4, 0, },
	/* I2C */
	{ HISTB_I2C0_CLK, "clk_i2c0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 4, 0, },
	{ HISTB_I2C1_CLK, "clk_i2c1", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 8, 0, },
	{ HISTB_I2C2_CLK, "clk_i2c2", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 12, 0, },
	/* SPI */
	{ HISTB_SPI0_CLK, "clk_spi0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x70, 0, 0, },
	/* SDIO */
	{ HISTB_SDIO0_BIU_CLK, "clk_sdio0_biu", "200m",
		CLK_SET_RATE_PARENT, 0x9c, 0, 0, },
	{ HISTB_SDIO0_CIU_CLK, "clk_sdio0_ciu", "sdio0_mux",
		CLK_SET_RATE_PARENT, 0x9c, 1, 0, },
	/* EMMC */
	{ HISTB_MMC_BIU_CLK, "clk_mmc_biu", "200m",
		CLK_SET_RATE_PARENT, 0xa0, 0, 0, },
	{ HISTB_MMC_CIU_CLK, "clk_mmc_ciu", "mmc_mux",
		CLK_SET_RATE_PARENT, 0xa0, 1, 0, },
	/* DMAC */
	{ HISTB_DMAC_CLK, "clk_dmac", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xa4, 0, 0, },
	/* USB2 */
	{ HISTB_USB2_BUS_CLK, "clk_u2_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb8, 0, 0, },
	{ HISTB_USB2_PHY_CLK, "clk_u2_phy", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 4, 0, },
	{ HISTB_USB2_12M_CLK, "clk_u2_12m", "12m",
		CLK_SET_RATE_PARENT, 0xb8, 2, 0 },
	{ HISTB_USB2_48M_CLK, "clk_u2_48m", "48m",
		CLK_SET_RATE_PARENT, 0xb8, 1, 0 },
	{ HISTB_USB2_UTMI_CLK, "clk_u2_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 5, 0 },
	{ HISTB_USB2_UTMI_CLK1, "clk_u2_utmi1", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 6, 0 },
	{ HISTB_USB2_OTG_UTMI_CLK, "clk_u2_otg_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 3, 0 },
	{ HISTB_USB2_PHY1_REF_CLK, "clk_u2_phy1_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 0, 0 },
	{ HISTB_USB2_PHY2_REF_CLK, "clk_u2_phy2_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 2, 0 },
	/* USB2 2 */
	{ HISTB_USB2_2_BUS_CLK, "clk_u2_2_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0x198, 0, 0, },
	{ HISTB_USB2_2_PHY_CLK, "clk_u2_2_phy", "60m",
		CLK_SET_RATE_PARENT, 0x198, 4, 0, },
	{ HISTB_USB2_2_12M_CLK, "clk_u2_2_12m", "12m",
		CLK_SET_RATE_PARENT, 0x198, 2, 0 },
	{ HISTB_USB2_2_48M_CLK, "clk_u2_2_48m", "48m",
		CLK_SET_RATE_PARENT, 0x198, 1, 0 },
	{ HISTB_USB2_2_UTMI_CLK, "clk_u2_2_utmi", "60m",
		CLK_SET_RATE_PARENT, 0x198, 5, 0 },
	{ HISTB_USB2_2_UTMI_CLK1, "clk_u2_2_utmi1", "60m",
		CLK_SET_RATE_PARENT, 0x198, 6, 0 },
	{ HISTB_USB2_2_OTG_UTMI_CLK, "clk_u2_2_otg_utmi", "60m",
		CLK_SET_RATE_PARENT, 0x198, 3, 0 },
	{ HISTB_USB2_2_PHY1_REF_CLK, "clk_u2_2_phy1_ref", "24m",
		CLK_SET_RATE_PARENT, 0x190, 0, 0 },
	{ HISTB_USB2_2_PHY2_REF_CLK, "clk_u2_2_phy2_ref", "24m",
		CLK_SET_RATE_PARENT, 0x190, 2, 0 },
	/* USB3 */
	{ HISTB_USB3_BUS_CLK, "clk_u3_bus", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 0, 0 },
	{ HISTB_USB3_UTMI_CLK, "clk_u3_utmi", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 4, 0 },
	{ HISTB_USB3_PIPE_CLK, "clk_u3_pipe", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 3, 0 },
	{ HISTB_USB3_SUSPEND_CLK, "clk_u3_suspend", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 2, 0 },
	/* GPU */
	{ HISTB_GPU_BUS_CLK, "clk_gpu", "200m",
		CLK_SET_RATE_PARENT, 0xd4, 0, 0 },
	/* FEPHY */
	{ HISTB_FEPHY_CLK, "clk_fephy", "25m",
		CLK_SET_RATE_PARENT, 0x120, 0, 0, },
};

static const struct hi3798_complex_clock hi3798mv100_complex_clks[] = {
	{ HISTB_ETH0_MAC_CLK, "clk_mac0", NULL,
		CLK_SET_RATE_PARENT, 0xcc, 0xf, 0xb, },
	{ HISTB_GPU_CORE_CLK, "clk_gpu_gp", "200m",
		CLK_SET_RATE_PARENT, 0xd4, 0x700, 0x700, },
};

static const struct hisi_clocks hi3798mv100_crg_clks = {
	.nr = HI3798_CRG_NR_CLKS,
	.fixed_rate_clks = hi3798_fixed_rate_clks,
	.fixed_rate_clks_num = ARRAY_SIZE(hi3798_fixed_rate_clks),
	.gate_clks = hi3798mv100_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798mv100_gate_clks),
	.mux_clks = hi3798mv100_mux_clks,
	.mux_clks_num = ARRAY_SIZE(hi3798mv100_mux_clks),
	.phase_clks = hi3798cv200_phase_clks,
	.phase_clks_num = ARRAY_SIZE(hi3798cv200_phase_clks),
	.customized_clks = hi3798mv100_complex_clks,
	.customized_clks_num = ARRAY_SIZE(hi3798mv100_complex_clks),
	.clk_register_customized = hi3798_clk_register_complex,
};

static const struct hisi_gate_clock hi3798mv100_sysctrl_gate_clks[] = {
	{ HISTB_IR_CLK, "clk_ir", "24m",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HISTB_TIMER01_CLK, "clk_timer01", "24m",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HISTB_UART0_CLK, "clk_uart0", "83p3m",
		CLK_SET_RATE_PARENT, 0x48, 12, 0, },
};

static const struct hisi_clocks hi3798mv100_sysctrl_clks = {
	.nr = HI3798_SYSCTRL_NR_CLKS,
	.gate_clks = hi3798mv100_sysctrl_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798mv100_sysctrl_gate_clks),
};

/* hi3798MV200 */

static struct hisi_mux_clock hi3798mv200_mux_clks[] = {
	{ HI3798_MMC_MUX, "mmc_mux", hi3798cv200_mmc_mux_p,
		ARRAY_SIZE(hi3798cv200_mmc_mux_p), CLK_SET_RATE_PARENT,
		0xa0, 8, 3, 0, hi3798cv200_mmc_mux_table, },
	{ HI3798_COMBPHY0_MUX, "combphy0_mux", hi3798cv200_comphy_mux_p,
		ARRAY_SIZE(hi3798cv200_comphy_mux_p), CLK_SET_RATE_PARENT,
		0x188, 3, 1, 0, hi3798cv200_comphy_mux_table, },
	{ HI3798_SDIO0_MUX, "sdio0_mux", hi3798cv200_sdio_mux_p,
		ARRAY_SIZE(hi3798cv200_sdio_mux_p), CLK_SET_RATE_PARENT,
		0x9c, 8, 2, 0, hi3798cv200_sdio_mux_table, },
};

static const struct hisi_gate_clock hi3798mv200_gate_clks[] = {
	/* UART */
	{ HISTB_UART2_CLK, "clk_uart2", "75m",
		CLK_SET_RATE_PARENT, 0x68, 4, 0, },
	/* I2C */
	{ HISTB_I2C0_CLK, "clk_i2c0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 4, 0, },
	{ HISTB_I2C1_CLK, "clk_i2c1", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 8, 0, },
	{ HISTB_I2C2_CLK, "clk_i2c2", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6C, 12, 0, },
	/* SPI */
	{ HISTB_SPI0_CLK, "clk_spi0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x70, 0, 0, },
	/* SDIO */
	{ HISTB_SDIO0_BIU_CLK, "clk_sdio0_biu", "200m",
			CLK_SET_RATE_PARENT, 0x9c, 0, 0, },
	{ HISTB_SDIO0_CIU_CLK, "clk_sdio0_ciu", "sdio0_mux",
		CLK_SET_RATE_PARENT, 0x9c, 1, 0, },
	/* EMMC */
	{ HISTB_MMC_BIU_CLK, "clk_mmc_biu", "200m",
		CLK_SET_RATE_PARENT, 0xa0, 0, 0, },
	{ HISTB_MMC_CIU_CLK, "clk_mmc_ciu", "mmc_mux",
		CLK_SET_RATE_PARENT, 0xa0, 1, 0, },
	{ HISTB_MMC_SAP_DLL_MODE_CLK, "clk_mmc_sap_dll_mode", "mmc_sample",
		CLK_SET_RATE_PARENT, 0x39c, 16, 0, },
	/* PCIE*/
	{ HISTB_PCIE_BUS_CLK, "clk_pcie_bus", "200m",
		CLK_SET_RATE_PARENT, 0x18c, 0, 0, },
	{ HISTB_PCIE_SYS_CLK, "clk_pcie_sys", "100m",
		CLK_SET_RATE_PARENT, 0x18c, 1, 0, },
	{ HISTB_PCIE_PIPE_CLK, "clk_pcie_pipe", "250m",
		CLK_SET_RATE_PARENT, 0x18c, 2, 0, },
	{ HISTB_PCIE_AUX_CLK, "clk_pcie_aux", "24m",
		CLK_SET_RATE_PARENT, 0x18c, 3, 0, },
	/* GSF */
	{ HISTB_ETH0_MAC_CLK, "clk_gsf", NULL,
		CLK_SET_RATE_PARENT, 0xcc, 1, 0, },
	{ HISTB_ETH0_MACIF_CLK, "clk_gmac", "clk_gsf",
		CLK_SET_RATE_PARENT, 0xcc, 3, 0, },
	/* FEPHY */
	{ HISTB_FEPHY_CLK, "clk_fephy", NULL,
		CLK_SET_RATE_PARENT, 0x388, 0, 0, },
	/* COMBPHY */
	{ HISTB_COMBPHY0_CLK, "clk_combphy0", "combphy0_mux",
		CLK_SET_RATE_PARENT, 0x188, 0, 0, },
	/* USB2 */
	{ HISTB_USB2_BUS_CLK, "clk_u2_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb8, 0, 0, },
	{ HISTB_USB2_PHY_CLK, "clk_u2_phy", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 4, 0, },
	{ HISTB_USB2_12M_CLK, "clk_u2_12m", "12m",
		CLK_SET_RATE_PARENT, 0xb8, 2, 0 },
	{ HISTB_USB2_48M_CLK, "clk_u2_48m", "48m",
		CLK_SET_RATE_PARENT, 0xb8, 1, 0 },
	{ HISTB_USB2_UTMI_CLK, "clk_u2_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 5, 0 },
	{ HISTB_USB2_OTG_UTMI_CLK, "clk_u2_otg_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 3, 0 },
	{ HISTB_USB2_PHY1_REF_CLK, "clk_u2_phy1_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 0, 0 },
	{ HISTB_USB2_PHY2_REF_CLK, "clk_u2_phy2_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 2, 0 },
	/* USB3 */
	{ HISTB_USB3_BUS_CLK, "clk_u3_bus", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 0, 0 },
	{ HISTB_USB3_UTMI_CLK, "clk_u3_utmi", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 4, 0 },
	{ HISTB_USB3_PIPE_CLK, "clk_u3_pipe", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 3, 0 },
	{ HISTB_USB3_SUSPEND_CLK, "clk_u3_suspend", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 2, 0 },
};

static const struct hi3798_complex_clock hi3798mv200_complex_clks[] = {
	{ HISTB_ETH1_MAC_CLK, "clk_mac0", NULL,
		CLK_SET_RATE_PARENT, 0xd0, 0xf, 0xb, },
};

static const struct hisi_clocks hi3798mv200_crg_clks = {
	.nr = HI3798_CRG_NR_CLKS,
	.fixed_rate_clks = hi3798_fixed_rate_clks,
	.fixed_rate_clks_num = ARRAY_SIZE(hi3798_fixed_rate_clks),
	.gate_clks = hi3798mv200_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798mv200_gate_clks),
	.mux_clks = hi3798mv200_mux_clks,
	.mux_clks_num = ARRAY_SIZE(hi3798mv200_mux_clks),
	.phase_clks = hi3798cv200_phase_clks,
	.phase_clks_num = ARRAY_SIZE(hi3798cv200_phase_clks),
	.customized_clks = hi3798mv200_complex_clks,
	.customized_clks_num = ARRAY_SIZE(hi3798mv200_complex_clks),
	.clk_register_customized = hi3798_clk_register_complex,
};

static const struct hisi_gate_clock hi3798mv200_sysctrl_gate_clks[] = {
	{ HISTB_IR_CLK, "clk_ir", "24m",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HISTB_TIMER01_CLK, "clk_timer01", "24m",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HISTB_UART0_CLK, "clk_uart0", "75m",
		CLK_SET_RATE_PARENT, 0x48, 12, 0, },
};

static const struct hisi_clocks hi3798mv200_sysctrl_clks = {
	.nr = HI3798_SYSCTRL_NR_CLKS,
	.gate_clks = hi3798mv200_sysctrl_gate_clks,
	.gate_clks_num = ARRAY_SIZE(hi3798mv200_sysctrl_gate_clks),
};

static const struct of_device_id hi3798_crg_match_table[] = {
	{ .compatible = "hisilicon,hi3798cv200-crg",
		.data = &hi3798cv200_crg_clks },
	{ .compatible = "hisilicon,hi3798cv200-sysctrl",
		.data = &hi3798cv200_sysctrl_clks },
	{ .compatible = "hisilicon,hi3798mv100-crg",
		.data = &hi3798mv100_crg_clks },
	{ .compatible = "hisilicon,hi3798mv100-sysctrl",
		.data = &hi3798mv100_sysctrl_clks },
	{ .compatible = "hisilicon,hi3798mv200-crg",
		.data = &hi3798mv200_crg_clks },
	{ .compatible = "hisilicon,hi3798mv200-sysctrl",
		.data = &hi3798mv200_sysctrl_clks },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798_crg_match_table);

static struct platform_driver hi3798_crg_driver = {
	.probe = hisi_crg_probe,
	.remove_new = hisi_crg_remove,
	.driver         = {
		.name   = "hi3798-crg",
		.of_match_table = hi3798_crg_match_table,
	},
};

module_platform_driver(hi3798_crg_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HiSilicon Hi3798 CRG Driver");
