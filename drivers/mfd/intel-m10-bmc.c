// SPDX-License-Identifier: GPL-2.0
/*
 * Intel MAX 10 Board Management Controller chip
 *
 * Copyright (C) 2018-2020 Intel Corporation. All rights reserved.
 */
#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

static struct mfd_cell m10bmc_bmc_subdevs[] = {
	{ .name = "d5005bmc-m10bmc" },
	{ .name = "d5005bmc-hwmon" },
	{ .name = "d5005bmc-secure" }
};

static struct mfd_cell m10bmc_pacn3000_subdevs[] = {
	{ .name = "n3000bmc-m10bmc" },
	{ .name = "n3000bmc-hwmon" },
	{ .name = "n3000bmc-retimer" },
	{ .name = "n3000bmc-secure" },
};

static void
m10bmc_init_cells_platdata(struct intel_m10bmc_platdata *pdata,
			   struct mfd_cell *cells, int n_cell)
{
	int i;

	for (i = 0; i < n_cell; i++) {
		if (!strcmp(cells[i].name, "n3000bmc-retimer")) {
			cells[i].platform_data = pdata->retimer;
			cells[i].pdata_size =
				pdata->retimer ? sizeof(*pdata->retimer) : 0;
		}
	}
}

static const struct regmap_range n3000_fw_handshake_regs[] = {
	regmap_reg_range(M10BMC_TELEM_START, M10BMC_TELEM_END),
};

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

static int check_m10bmc_version(struct intel_m10bmc *ddata)
{
	unsigned int v;
	int ret;

	/*
	 * This check is to filter out the very old legacy BMC versions,
	 * M10BMC_LEGACY_SYS_BASE is the offset to this old block of mmio
	 * registers. In the old BMC chips, the BMC version info is stored
	 * in this old version register (M10BMC_LEGACY_SYS_BASE +
	 * M10BMC_BUILD_VER), so its read out value would have not been
	 * LEGACY_INVALID (0xffffffff). But in new BMC chips that the
	 * driver supports, the value of this register should be
	 * LEGACY_INVALID.
	 */
	ret = m10bmc_raw_read(ddata,
			      M10BMC_LEGACY_SYS_BASE + M10BMC_BUILD_VER, &v);
	if (ret)
		return -ENODEV;

	if (v != M10BMC_VER_LEGACY_INVALID) {
		dev_err(ddata->dev, "bad version M10BMC detected\n");
		return -ENODEV;
	}

	return 0;
}

static int intel_m10_bmc_spi_probe(struct spi_device *spi)
{
	struct intel_m10bmc_platdata *pdata = dev_get_platdata(&spi->dev);
	const struct spi_device_id *id = spi_get_device_id(spi);
	struct device *dev = &spi->dev;
	struct mfd_cell *cells;
	struct intel_m10bmc *ddata;
	int ret, n_cell;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	init_rwsem(&ddata->bmcfw_lock);
	ddata->dev = dev;

	ddata->regmap =
		devm_regmap_init_spi_avmm(spi, &intel_m10bmc_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		ret = PTR_ERR(ddata->regmap);
		dev_err(dev, "Failed to allocate regmap: %d\n", ret);
		return ret;
	}

	spi_set_drvdata(spi, ddata);

	ret = check_m10bmc_version(ddata);
	if (ret) {
		dev_err(dev, "Failed to identify m10bmc hardware\n");
		return ret;
	}

	switch (id->driver_data) {
	case M10_N3000:
		cells = m10bmc_pacn3000_subdevs;
		n_cell = ARRAY_SIZE(m10bmc_pacn3000_subdevs);
		break;
	case M10_D5005:
		cells = m10bmc_bmc_subdevs;
		n_cell = ARRAY_SIZE(m10bmc_bmc_subdevs);
		break;
	default:
		return -ENODEV;
	}

	m10bmc_init_cells_platdata(pdata, cells, n_cell);

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, cells, n_cell,
				   NULL, 0, NULL);
	if (ret)
		dev_err(dev, "Failed to register sub-devices: %d\n", ret);

	return ret;
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

MODULE_DESCRIPTION("Intel MAX 10 BMC Device Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:intel-m10-bmc");
