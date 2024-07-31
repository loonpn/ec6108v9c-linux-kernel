// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Driver for HiSilicon Festa PHYs
 *
 * This module does nothing than firmware injection. If you don't use firmware,
 * simply blacklist this module.
 *
 * Copyright (c) 2023 David Yang
 */
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/phy.h>

#define PHY_ID_HISILICON_FESTAV200	0x20669813
#define PHY_ID_HISILICON_FESTAV220	0x20669823
#define PHY_ID_HISILICON_FESTAV300	0x20669833
#define PHY_ID_HISILICON_FESTAV320	0x20669843
#define PHY_ID_HISILICON_FESTAV330	0x20669853
#define PHY_ID_HISILICON_FESTAV331	0x20669863

#define MII_EXPMD	0x1d	/* Expanded memory data */
#define MII_EXPMA	0x1e	/* Expanded memory address */

/* bus->mdio_lock should be locked when using this function */
static int hisi_festa_write_expanded(struct phy_device *phydev, u16 addr, u8 val)
{
	__phy_write(phydev, MII_EXPMA, addr);
	__phy_write(phydev, MII_EXPMD, val);
	return 0;
}

/* bus->mdio_lock should be locked when using this function */
static int hisi_festa_write_expanded_mem(struct phy_device *phydev, u16 addr,
					 const u8 *data, int len)
{
	int i;

	for (i = 0; i < len; i++)
		hisi_festa_write_expanded(phydev, addr + i, data[i]);
	return 0;
}

static int hisi_festa_write_fw(struct phy_device *phydev, const struct firmware *fw)
{
	static const u8 prologue[] = {0xbd, 0x34, 0x00, 0x39};
	int ret;

	phy_lock_mdio_bus(phydev);

	ret = __phy_set_bits(phydev, MII_BMCR, BMCR_PDOWN);
	if (ret) {
		phydev_err(phydev, "cannot suspend device\n");
		goto out;
	}

	hisi_festa_write_expanded_mem(phydev, 0x33f9, prologue, sizeof(prologue));
	/* mask jump instruction */
	hisi_festa_write_expanded(phydev, 0x3400, 0x39);
	hisi_festa_write_expanded_mem(phydev, 0x3401, fw->data + 1, fw->size - 1);
	/* now release firmware */
	hisi_festa_write_expanded(phydev, 0x3400, fw->data[0]);
	hisi_festa_write_expanded(phydev, 0x33f8, 0x01);

	ret = __phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);

out:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

static int hisi_festa_patch_fw(struct phy_device *phydev)
{
	int ret;
	char fw_name[64];
	const struct firmware *fw;

	snprintf(fw_name, sizeof(fw_name), "hisilicon/festa.%08x.ucode", phydev->phy_id);

	ret = request_firmware(&fw, fw_name, &phydev->mdio.dev);
	if (ret) {
		/* err message already printed by request_firmware */
		return -EAGAIN;
	}

	if (fw->data[0] != 0x01 || fw->data[1] != 0xcc) {
		phydev_err(phydev, "%s does not look like valid firmware; refused to load\n",
			   fw_name);
		ret = -EINVAL;
		goto out;
	}

	ret = hisi_festa_write_fw(phydev, fw);
	if (ret) {
		phydev_err(phydev, "download firmware %s failed\n", fw_name);
		goto out;
	}

	phydev_info(phydev, "using firmware %s\n", fw_name);

out:
	release_firmware(fw);
	return ret;
}

static int hisi_festa_config_init(struct phy_device *phydev)
{
	hisi_festa_patch_fw(phydev);
	/* ok, use programmed firmware */
	return 0;
}

static struct phy_driver hisi_festa_driver[] = {
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV200),
		.name        = "HiSilicon Festa v200/v210",
		.config_init = hisi_festa_config_init,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV220),
		.name        = "HiSilicon Festa v220",
		.config_init = hisi_festa_config_init,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV300),
		.name        = "HiSilicon Festa v300",
		.config_init = hisi_festa_config_init,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV320),
		.name        = "HiSilicon Festa v320",
		.config_init = hisi_festa_config_init,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV330),
		.name        = "HiSilicon Festa v330",
		.config_init = hisi_festa_config_init,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV331),
		.name        = "HiSilicon Festa v331",
		.config_init = hisi_festa_config_init,
	},
};

module_phy_driver(hisi_festa_driver);

static struct mdio_device_id __maybe_unused hisi_festa_tbl[] = {
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV200) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV220) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV300) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV320) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV330) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV331) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, hisi_festa_tbl);
MODULE_DESCRIPTION("HiSilicon Festa PHY driver");
MODULE_LICENSE("Dual MIT/GPL");
