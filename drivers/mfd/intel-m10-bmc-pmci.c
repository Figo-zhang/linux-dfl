// SPDX-License-Identifier: GPL-2.0
/*
 * PMCI-based interface to MAX10 BMC
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
	struct intel_m10bmc m10bmc;
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

static const struct attribute_group *m10bmc_pmci_groups[] = {
	&m10bmc_group,
};

static const struct m10bmc_csr pmci_m10bmc_csr = {
	.base = PMCI_M10BMC_SYS_BASE,
	.build_version = PMCI_M10BMC_BUILD_VER,
	.fw_version = PMCI_NIOS2_FW_VERSION,
	.mac_addr1 = PMCI_M10BMC_MACADDR1,
	.mac_addr2 = PMCI_M10BMC_MACADDR2,
};

static int pmci_probe(struct dfl_device *ddev)
{
	struct device *dev = &ddev->dev;
	struct pmci_device *pmci;

	pmci = devm_kzalloc(dev, sizeof(*pmci), GFP_KERNEL);
	if (!pmci)
		return -ENOMEM;

	pmci->m10bmc.dev = dev;
	pmci->dev = dev;
	pmci->m10bmc.type = M10_PMCI;
	pmci->m10bmc.csr = &pmci_m10bmc_csr;

	pmci->base = devm_ioremap_resource(dev, &ddev->mmio_res);
	if (IS_ERR(pmci->base))
		return PTR_ERR(pmci->base);

	pmci->m10bmc.regmap =
		devm_regmap_init_indirect_register(dev,
						   pmci->base + PMCI_SPI_BASE_OFF,
						   &pmci_max10_cfg);
	if (IS_ERR(pmci->m10bmc.regmap))
		return PTR_ERR(pmci->m10bmc.regmap);

	return m10bmc_dev_init(&pmci->m10bmc);
}

#define FME_FEATURE_ID_PMCI_BMC	0x12

static const struct dfl_device_id pmci_ids[] = {
	{ FME_ID, FME_FEATURE_ID_PMCI_BMC },
	{ }
};
MODULE_DEVICE_TABLE(dfl, pmci_ids);

static struct dfl_driver pmci_driver = {
	.drv	= {
		.name       = "dfl-pmci",
		.dev_groups = m10bmc_pmci_groups,
	},
	.id_table = pmci_ids,
	.probe   = pmci_probe,
};

module_dfl_driver(pmci_driver);

MODULE_DESCRIPTION("MAX10 BMC PMCI-based interface");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
