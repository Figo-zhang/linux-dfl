// SPDX-License-Identifier: GPL-2.0
/*
 * SPI bus interface to Intel MAX 10 Board Management Controller
 *
 * Copyright (C) 2018-2021 Intel Corporation. All rights reserved.
 */
#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

static const struct regmap_range m10_regmap_range[] = {
	regmap_reg_range(M10BMC_LEGACY_SYS_BASE, M10BMC_SYS_END),
	regmap_reg_range(M10BMC_FLASH_BASE, M10BMC_MEM_END),
};

static const struct regmap_access_table m10_access_table = {
	.yes_ranges	= m10_regmap_range,
	.n_yes_ranges	= ARRAY_SIZE(m10_regmap_range),
};

static struct regmap_config intel_m10bmc_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.wr_table = &m10_access_table,
	.rd_table = &m10_access_table,
	.max_register = M10BMC_MEM_END,
};

static int intel_m10_bmc_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *id = spi_get_device_id(spi);
	struct device *dev = &spi->dev;
	struct m10bmc_dev *mdev;
	int ret;

	mdev = devm_kzalloc(dev, sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;

	mdev->dev = dev;
	mdev->type = (enum m10bmc_type)id->driver_data;

	mdev->regmap =
		devm_regmap_init_spi_avmm(spi, &intel_m10bmc_regmap_config);
	if (IS_ERR(mdev->regmap)) {
		ret = PTR_ERR(mdev->regmap);
		dev_err(dev, "Failed to allocate regmap: %d\n", ret);
		return ret;
	}

	return m10bmc_dev_init(mdev);
}

static const struct spi_device_id m10bmc_spi_id[] = {
	{ "m10-n3000", M10_N3000 },
	{ "m10-d5005", M10_D5005 },
	{ }
};
MODULE_DEVICE_TABLE(spi, m10bmc_spi_id);

static struct spi_driver intel_m10bmc_spi_driver = {
	.driver = {
		.name = "intel-m10-bmc",
	},
	.probe = intel_m10_bmc_spi_probe,
	.id_table = m10bmc_spi_id,
};
module_spi_driver(intel_m10bmc_spi_driver);

MODULE_DESCRIPTION("MAX10 BMC SPI bus interface");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:intel-m10-bmc");
