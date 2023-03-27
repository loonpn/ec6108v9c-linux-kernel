// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hi3798 Clock and Reset Generator Driver
 *
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 */

#include <dt-bindings/clock/histb-clock.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "clk.h"
#include "crg.h"
#include "reset.h"

/* hi3798 core CRG */
#define HI3798_INNER_CLK_OFFSET		64
#define HI3798_FIXED_24M			65
#define HI3798_FIXED_25M			66
#define HI3798_FIXED_50M			67
#define HI3798_FIXED_75M			68
#define HI3798_FIXED_100M			69
#define HI3798_FIXED_150M			70
#define HI3798_FIXED_200M			71
#define HI3798_FIXED_250M			72
#define HI3798_FIXED_300M			73
#define HI3798_FIXED_400M			74
#define HI3798_MMC_MUX			75
#define HI3798_ETH_PUB_CLK			76
#define HI3798_ETH_BUS_CLK			77
#define HI3798_ETH_BUS0_CLK		78
#define HI3798_ETH_BUS1_CLK		79
#define HI3798_COMBPHY1_MUX		80
#define HI3798_FIXED_12M			81
#define HI3798_FIXED_48M			82
#define HI3798_FIXED_60M			83
#define HI3798_FIXED_166P5M		84
#define HI3798_SDIO0_MUX			85
#define HI3798_COMBPHY0_MUX		86
#define HI3798_FIXED_3M				87
#define HI3798_FIXED_15M			88
#define HI3798_FIXED_83P3M			89
#define HI3798_GPU_PP0_CLK			90
#define HI3798_GPU_PP1_CLK			91

#define HI3798_CRG_NR_CLKS			128

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
};

struct hi3798_clk_complex {
	struct clk_hw	hw;
	void __iomem	*reg;
	u32		mask;
};

#define to_complex_clk(_hw) container_of(_hw, struct hi3798_clk_complex, hw)

static int clk_complex_prepare(struct clk_hw *hw)
{
	struct hi3798_clk_complex *clk = to_complex_clk(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	val |= clk->mask;
	writel_relaxed(val, clk->reg);

	return 0;
}

static void clk_complex_unprepare(struct clk_hw *hw)
{
	struct hi3798_clk_complex *clk = to_complex_clk(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	val &= ~(clk->mask);
	writel_relaxed(val, clk->reg);
}

static const struct clk_ops clk_complex_ops = {
	.prepare = clk_complex_prepare,
	.unprepare = clk_complex_unprepare,
};

static int hi3798_clk_register_complex(const struct hi3798_complex_clock *clks, int nums,
				       struct hisi_clock_data *data)
{
	void __iomem *base = data->base;
	int i;
	int ret;

	for (i = 0; i < nums; i++) {
		struct hi3798_clk_complex *p_clk;
		struct clk *clk;
		struct clk_init_data init;

		p_clk = kzalloc(sizeof(*p_clk), GFP_KERNEL);
		if (!p_clk) {
			ret = -ENOMEM;
			goto err_kzalloc;
		}

		init.name = clks[i].name;
		init.ops = &clk_complex_ops;

		init.flags = 0;
		init.parent_names =
			(clks[i].parent_name ? &clks[i].parent_name : NULL);
		init.num_parents = (clks[i].parent_name ? 1 : 0);

		p_clk->reg = base + clks[i].offset;
		p_clk->mask = clks[i].mask;
		p_clk->hw.init = &init;

		ret = clk_hw_register(NULL, &p_clk->hw);
		if (ret) {
			kfree(p_clk);
err_kzalloc:
			pr_err("%s: failed to register clock %s\n",
			       __func__, clks[i].name);
			goto err;
		}

		data->clk_data.clks[clks[i].id] = clk;
	}

	return 0;

err:
	while (i--)
		clk_unregister(data->clk_data.clks[clks[i].id]);

	return ret;
}

static void hi3798_clk_unregister_complex(const struct hi3798_complex_clock *clks, int nums,
					  struct hisi_clock_data *data)
{
	struct clk **clocks = data->clk_data.clks;
	int i;
	for (i = 0; i < nums; i++) {
		int id = clks[i].id;
		if (clocks[id])
			clk_unregister(clocks[id]);
	}
}

struct hi3798_clks {
	const struct hisi_gate_clock *gate_clks;
	int gate_clks_nums;
	const struct hisi_mux_clock *mux_clks;
	int mux_clks_nums;
	const struct hisi_phase_clock *phase_clks;
	int phase_clks_nums;
	const struct hi3798_complex_clock *complex_clks;
	int complex_clks_nums;
};

static struct hisi_clock_data *hi3798_clk_register(
		struct platform_device *pdev, const struct hi3798_clks *clks)
{
	struct hisi_clock_data *clk_data;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798_CRG_NR_CLKS);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	/* hisi_phase_clock is resource managed */
	ret = hisi_clk_register_phase(&pdev->dev, clks->phase_clks,
				      clks->phase_clks_nums, clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_fixed_rate(hi3798_fixed_rate_clks,
					   ARRAY_SIZE(hi3798_fixed_rate_clks),
					   clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_mux(clks->mux_clks, clks->mux_clks_nums, clk_data);
	if (ret)
		goto unregister_fixed_rate;

	ret = hisi_clk_register_gate(clks->gate_clks, clks->gate_clks_nums, clk_data);
	if (ret)
		goto unregister_mux;

	ret = hi3798_clk_register_complex(clks->complex_clks, clks->complex_clks_nums, clk_data);
	if (ret)
		goto unregister_gate;

	ret = of_clk_add_provider(pdev->dev.of_node,
			of_clk_src_onecell_get, &clk_data->clk_data);
	if (ret)
		goto unregister_complex;

	return clk_data;

unregister_complex:
	hi3798_clk_unregister_complex(clks->complex_clks, clks->complex_clks_nums, clk_data);
unregister_gate:
	hisi_clk_unregister_gate(clks->gate_clks, clks->gate_clks_nums, clk_data);
unregister_mux:
	hisi_clk_unregister_mux(clks->mux_clks, clks->mux_clks_nums, clk_data);
unregister_fixed_rate:
	hisi_clk_unregister_fixed_rate(hi3798_fixed_rate_clks,
				       ARRAY_SIZE(hi3798_fixed_rate_clks),
				       clk_data);
	return ERR_PTR(ret);
}

static void hi3798_clk_unregister(
		struct platform_device *pdev, const struct hi3798_clks *clks)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	of_clk_del_provider(pdev->dev.of_node);

	hi3798_clk_unregister_complex(clks->complex_clks, clks->complex_clks_nums, crg->clk_data);
	hisi_clk_unregister_gate(clks->gate_clks, clks->gate_clks_nums, crg->clk_data);
	hisi_clk_unregister_mux(clks->mux_clks, clks->mux_clks_nums, crg->clk_data);
	hisi_clk_unregister_fixed_rate(hi3798_fixed_rate_clks,
				       ARRAY_SIZE(hi3798_fixed_rate_clks),
				       crg->clk_data);
}

/* hi3798 sysctrl CRG */

#define HI3798_SYSCTRL_NR_CLKS 16

static struct hisi_clock_data *hi3798_sysctrl_clk_register(
		struct platform_device *pdev, const struct hi3798_clks *clks)
{
	struct hisi_clock_data *clk_data;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798_SYSCTRL_NR_CLKS);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	ret = hisi_clk_register_gate(clks->gate_clks, clks->gate_clks_nums, clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = of_clk_add_provider(pdev->dev.of_node,
			of_clk_src_onecell_get, &clk_data->clk_data);
	if (ret)
		goto unregister_gate;

	return clk_data;

unregister_gate:
	hisi_clk_unregister_gate(clks->gate_clks, clks->gate_clks_nums, clk_data);
	return ERR_PTR(ret);
}

static void hi3798_sysctrl_clk_unregister(
		struct platform_device *pdev, const struct hi3798_clks *clks)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	of_clk_del_provider(pdev->dev.of_node);

	hisi_clk_unregister_gate(clks->gate_clks, clks->gate_clks_nums, crg->clk_data);
}

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

static u32 mmc_phase_regvals[] = {0, 1, 2, 3, 4, 5, 6, 7};
static u32 mmc_phase_degrees[] = {0, 45, 90, 135, 180, 225, 270, 315};

static struct hisi_phase_clock hi3798mv100_phase_clks[] = {
	{ HISTB_MMC_SAMPLE_CLK, "mmc_sample", "clk_mmc_ciu",
		CLK_SET_RATE_PARENT, 0xa0, 12, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HISTB_MMC_DRV_CLK, "mmc_drive", "clk_mmc_ciu",
		CLK_SET_RATE_PARENT, 0xa0, 16, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
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
	/* Ethernet */
	{ HI3798_ETH_BUS_CLK, "clk_bus", NULL,
		CLK_SET_RATE_PARENT, 0xcc, 0, 0, },
	{ HI3798_ETH_PUB_CLK, "clk_pub", "clk_bus",
		CLK_SET_RATE_PARENT, 0xcc, 1, 0, },
	{ HISTB_ETH0_MAC_CLK, "clk_mac0", "clk_pub",
		CLK_SET_RATE_PARENT, 0xcc, 3, 0, },
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
	{ HISTB_GPU_GP_CLK, "clk_gpu_gp", "clk_gpu_pp0",
		CLK_SET_RATE_PARENT, 0xd4, 8, 0 },
	{ HI3798_GPU_PP0_CLK, "clk_gpu_pp0", "clk_gpu_pp1",
		CLK_SET_RATE_PARENT, 0xd4, 9, 0 },
	{ HI3798_GPU_PP1_CLK, "clk_gpu_pp1", "200m",
		CLK_SET_RATE_PARENT, 0xd4, 10, 0 },
	/* FEPHY */
	{ HISTB_FEPHY_CLK, "clk_fephy", "25m",
		CLK_SET_RATE_PARENT, 0x120, 0, 0, },
};

static const struct hi3798_complex_clock hi3798mv100_complex_clks[] = {
	//{ HIX5HD2_MAC0_CLK, "clk_mac0", "clk_fephy", 0xcc, 0xa },
};

static const struct hi3798_clks hi3798mv100_crg_clks = {
	.gate_clks = hi3798mv100_gate_clks,
	.gate_clks_nums = ARRAY_SIZE(hi3798mv100_gate_clks),
	.mux_clks = hi3798mv100_mux_clks,
	.mux_clks_nums = ARRAY_SIZE(hi3798mv100_mux_clks),
	.phase_clks = hi3798mv100_phase_clks,
	.phase_clks_nums = ARRAY_SIZE(hi3798mv100_phase_clks),
	.complex_clks = hi3798mv100_complex_clks,
	.complex_clks_nums = ARRAY_SIZE(hi3798mv100_complex_clks),
};

static struct hisi_clock_data *hi3798mv100_clk_register(
				struct platform_device *pdev)
{
	return hi3798_clk_register(pdev, &hi3798mv100_crg_clks);
}

static void hi3798mv100_clk_unregister(struct platform_device *pdev)
{
	hi3798_clk_unregister(pdev, &hi3798mv100_crg_clks);
}

static const struct hisi_crg_funcs hi3798mv100_crg_funcs = {
	.register_clks = hi3798mv100_clk_register,
	.unregister_clks = hi3798mv100_clk_unregister,
};

static const struct hisi_gate_clock hi3798mv100_sysctrl_gate_clks[] = {
	{ HISTB_IR_CLK, "clk_ir", "24m",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HISTB_TIMER01_CLK, "clk_timer01", "24m",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HISTB_UART0_CLK, "clk_uart0", "83p3m",
		CLK_SET_RATE_PARENT, 0x48, 12, 0, },
};

static const struct hi3798_clks hi3798mv100_sysctrl_clks = {
	.gate_clks = hi3798mv100_sysctrl_gate_clks,
	.gate_clks_nums = ARRAY_SIZE(hi3798mv100_sysctrl_gate_clks),
};

static struct hisi_clock_data *hi3798mv100_sysctrl_clk_register(
					struct platform_device *pdev)
{
	return hi3798_sysctrl_clk_register(pdev, &hi3798mv100_sysctrl_clks);
}

static void hi3798mv100_sysctrl_clk_unregister(struct platform_device *pdev)
{
	hi3798_sysctrl_clk_unregister(pdev, &hi3798mv100_sysctrl_clks);
}

static const struct hisi_crg_funcs hi3798mv100_sysctrl_funcs = {
	.register_clks = hi3798mv100_sysctrl_clk_register,
	.unregister_clks = hi3798mv100_sysctrl_clk_unregister,
};

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

static const struct hi3798_clks hi3798cv200_crg_clks = {
	.gate_clks = hi3798cv200_gate_clks,
	.gate_clks_nums = ARRAY_SIZE(hi3798cv200_gate_clks),
	.mux_clks = hi3798cv200_mux_clks,
	.mux_clks_nums = ARRAY_SIZE(hi3798cv200_mux_clks),
	.phase_clks = hi3798mv100_phase_clks,
	.phase_clks_nums = ARRAY_SIZE(hi3798mv100_phase_clks),
};

static struct hisi_clock_data *hi3798cv200_clk_register(
				struct platform_device *pdev)
{
	return hi3798_clk_register(pdev, &hi3798cv200_crg_clks);
}

static void hi3798cv200_clk_unregister(struct platform_device *pdev)
{
	hi3798_clk_unregister(pdev, &hi3798cv200_crg_clks);
}

static const struct hisi_crg_funcs hi3798cv200_crg_funcs = {
	.register_clks = hi3798cv200_clk_register,
	.unregister_clks = hi3798cv200_clk_unregister,
};

static const struct hisi_gate_clock hi3798cv200_sysctrl_gate_clks[] = {
	{ HISTB_IR_CLK, "clk_ir", "24m",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HISTB_TIMER01_CLK, "clk_timer01", "24m",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HISTB_UART0_CLK, "clk_uart0", "75m",
		CLK_SET_RATE_PARENT, 0x48, 10, 0, },
};

static const struct hi3798_clks hi3798cv200_sysctrl_clks = {
	.gate_clks = hi3798cv200_sysctrl_gate_clks,
	.gate_clks_nums = ARRAY_SIZE(hi3798cv200_sysctrl_gate_clks),
};

static struct hisi_clock_data *hi3798cv200_sysctrl_clk_register(
					struct platform_device *pdev)
{
	return hi3798_sysctrl_clk_register(pdev, &hi3798cv200_sysctrl_clks);
}

static void hi3798cv200_sysctrl_clk_unregister(struct platform_device *pdev)
{
	hi3798_sysctrl_clk_unregister(pdev, &hi3798cv200_sysctrl_clks);
}

static const struct hisi_crg_funcs hi3798cv200_sysctrl_funcs = {
	.register_clks = hi3798cv200_sysctrl_clk_register,
	.unregister_clks = hi3798cv200_sysctrl_clk_unregister,
};

static const struct of_device_id hi3798_crg_match_table[] = {
	{ .compatible = "hisilicon,hi3798mv100-crg",
		.data = &hi3798mv100_crg_funcs },
	{ .compatible = "hisilicon,hi3798mv100-sysctrl",
		.data = &hi3798mv100_sysctrl_funcs },
	{ .compatible = "hisilicon,hi3798cv200-crg",
		.data = &hi3798cv200_crg_funcs },
	{ .compatible = "hisilicon,hi3798cv200-sysctrl",
		.data = &hi3798cv200_sysctrl_funcs },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798_crg_match_table);

static int hi3798_crg_probe(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg;

	crg = devm_kmalloc(&pdev->dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;

	crg->funcs = of_device_get_match_data(&pdev->dev);
	if (!crg->funcs)
		return -ENOENT;

	crg->rstc = hisi_reset_init(pdev);
	if (!crg->rstc)
		return -ENOMEM;

	crg->clk_data = crg->funcs->register_clks(pdev);
	if (IS_ERR(crg->clk_data)) {
		hisi_reset_exit(crg->rstc);
		return PTR_ERR(crg->clk_data);
	}

	platform_set_drvdata(pdev, crg);
	return 0;
}

static int hi3798_crg_remove(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	hisi_reset_exit(crg->rstc);
	crg->funcs->unregister_clks(pdev);
	return 0;
}

static struct platform_driver hi3798_crg_driver = {
	.probe = hi3798_crg_probe,
	.remove = hi3798_crg_remove,
	.driver = {
		.name = "hi3798-crg",
		.of_match_table = hi3798_crg_match_table,
	},
};

static int __init hi3798_crg_init(void)
{
	return platform_driver_register(&hi3798_crg_driver);
}
core_initcall(hi3798_crg_init);

static void __exit hi3798_crg_exit(void)
{
	platform_driver_unregister(&hi3798_crg_driver);
}
module_exit(hi3798_crg_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("HiSilicon Hi3798 CRG Driver");
