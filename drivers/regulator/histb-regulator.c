// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Device driver for regulators in HiSTB SoCs
 *
 * Copyright (c) 2023 David Yang
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/rational.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

/*
 * duty + 1   = reg & 0xffff0000
 * period + 1 = reg & 0x0000ffff
 * clock: xtal (usually 24M)
 */

/* 24000 kHz / 4094 ~= 6 kHz ~= 0.17 ms */
#define HISTB_REGULATOR_MAX_PERIOD 0xfff

struct histb_regulator {
	struct regulator_desc desc;
	void __iomem *base;
	int min_uV;
	int max_uV;
};

static int histb_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct histb_regulator *reg = rdev_get_drvdata(rdev);
	u32 val;
	u32 duty;
	u32 period;

	val = readl_relaxed(reg->base);
	duty   = (val & 0xffff0000) >> 16;
	period = (val & 0x0000ffff) >>  0;

	if (duty <= 1 || period <= 1)
		return reg->max_uV;
	if (duty >= period)
		return reg->min_uV;

	duty -= 1;
	period -= 1;

	return reg->max_uV - (reg->max_uV - reg->min_uV) * duty / period;
}

static int histb_regulator_set_voltage(struct regulator_dev *rdev,
				       int min_uV, int max_uV, unsigned int *selector)
{
	struct histb_regulator *reg = rdev_get_drvdata(rdev);
	unsigned long duty;
	unsigned long period;

	if (min_uV >= reg->max_uV) {
		writel_relaxed(0x00010002, reg->base);
		return 0;
	}
	if (min_uV <= reg->min_uV) {
		writel_relaxed(0x00020002, reg->base);
		return 0;
	}

	rational_best_approximation(reg->max_uV - min_uV, reg->max_uV - reg->min_uV,
				    HISTB_REGULATOR_MAX_PERIOD, HISTB_REGULATOR_MAX_PERIOD,
				    &duty, &period);
	writel_relaxed(((duty + 1) << 16) | (period + 1), reg->base);

	return 0;
}

static const struct regulator_ops histb_regulator_ops = {
	.get_voltage = histb_regulator_get_voltage,
	.set_voltage = histb_regulator_set_voltage,
};

static int histb_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct histb_regulator *reg;
	struct regulator_desc *desc;
	struct regulator_init_data *init_data;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	int ret;

	reg = devm_kzalloc(dev, sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	desc = &reg->desc;

	init_data = of_get_regulator_init_data(dev, np, desc);
	if (!init_data)
		return -ENOMEM;

	reg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg->base))
		return -ENOMEM;

	desc->name = dev_name(&pdev->dev);
	desc->type = REGULATOR_VOLTAGE;
	desc->owner = THIS_MODULE;
	desc->ops = &histb_regulator_ops;
	desc->continuous_voltage_range = true;

	reg->min_uV = init_data->constraints.min_uV;
	reg->max_uV = init_data->constraints.max_uV;

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = reg;
	config.of_node = np;

	rdev = devm_regulator_register(dev, desc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		dev_err(dev, "failed to register %s (%d)\n", desc->name, ret);
		return ret;
	}

	platform_set_drvdata(pdev, reg);
	return 0;
}

static const struct of_device_id histb_regulator_of_match[] = {
	{ .compatible = "hisilicon,histb-volt", },
	{ }
};

static struct platform_driver histb_regulator_driver = {
	.probe = histb_regulator_probe,
	.driver = {
		.name = "histb-regulator",
		.of_match_table = of_match_ptr(histb_regulator_of_match),
	},
};

module_platform_driver(histb_regulator_driver);

MODULE_DESCRIPTION("HiSTB regulator");
MODULE_LICENSE("Dual MIT/GPL");
