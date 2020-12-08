// SPDX-License-Identifier: GPL-2.0
/*
 * DFL device driver for PMCI subsystem private feature.
 *
 * Copyright (C) 2020-2021 Intel Corporation, Inc.
 *
 */
#include <linux/bitfield.h>
#include <linux/dfl.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define PMCI_SPI_BASE_OFF 0x100

struct pmci_device {
	void __iomem *base;
	struct device *dev;
	struct intel_pmci_secure_pdata pdata;
};

static struct mfd_cell pmci_subdevs[] = {
	{ .name = "intel-pmci-hwmon" },
	{ .name = "intel-pmci-secure" }
};

static const struct regmap_range m10_regmap_range[] = {
	regmap_reg_range(PMCI_M10BMC_SYS_BASE, PMCI_M10BMC_SYS_END),
};

static const struct regmap_access_table m10_access_table = {
	.yes_ranges	= m10_regmap_range,
	.n_yes_ranges	= ARRAY_SIZE(m10_regmap_range),
};

static const struct regmap_config pmci_max10_cfg = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.wr_table = &m10_access_table,
	.rd_table = &m10_access_table,
	.max_register = PMCI_M10BMC_SYS_END,
};

static void
pmci_init_cells_platdata(struct pmci_device *pmci,
			   struct mfd_cell *cells, int n_cell)
{
	int i;

	for (i = 0; i < n_cell; i++) {
		if (!strcmp(cells[i].name, "intel-pmci-secure")) {
			cells[i].platform_data = &pmci->pdata;
			cells[i].pdata_size = sizeof(pmci->pdata);
		}
	}
}

static int pmci_probe(struct dfl_device *ddev)
{
	struct device *dev = &ddev->dev;
	struct pmci_device *pmci;
	struct mfd_cell *cells;
	struct intel_m10bmc *ddata;
	int ret, n_cell;

	pmci = devm_kzalloc(dev, sizeof(*pmci), GFP_KERNEL);
	if (!pmci)
		return -ENOMEM;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->dev = dev;
	pmci->dev = dev;

	pmci->base = devm_ioremap_resource(&ddev->dev, &ddev->mmio_res);
	if (IS_ERR(pmci->base))
		return PTR_ERR(pmci->base);

	pmci->pdata.base = pmci->base;

	ddata->regmap = devm_regmap_init_indirect_register(dev,
			pmci->base + PMCI_SPI_BASE_OFF, &pmci_max10_cfg);
	if (IS_ERR(ddata->regmap))
		return PTR_ERR(ddata->regmap);

	dev_set_drvdata(&ddev->dev, ddata);

	cells = pmci_subdevs;
	n_cell = ARRAY_SIZE(pmci_subdevs);

	pmci_init_cells_platdata(pmci, cells, n_cell);

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, cells, n_cell,
				   NULL, 0, NULL);
	if (ret)
		dev_err(dev, "Failed to register sub-devices: %d\n", ret);

	return ret;
}

#define FME_FEATURE_ID_PMCI_BMC	0xd

static const struct dfl_device_id pmci_ids[] = {
	{ FME_ID, FME_FEATURE_ID_PMCI_BMC },
	{ }
};
MODULE_DEVICE_TABLE(dfl, pmci_ids);

static struct dfl_driver pmci_driver = {
	.drv	= {
		.name       = "dfl-pmci",
	},
	.id_table = pmci_ids,
	.probe   = pmci_probe,
};

module_dfl_driver(pmci_driver);

MODULE_DESCRIPTION("Intel PMCI Device Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
