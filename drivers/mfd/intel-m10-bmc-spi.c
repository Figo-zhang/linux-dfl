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

static ssize_t bmc_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *ddata = dev_get_drvdata(dev);

	return m10bmc_show_bmc_version(ddata, buf);
}
static DEVICE_ATTR_RO(bmc_version);

static ssize_t bmcfw_version_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *ddata = dev_get_drvdata(dev);

	return m10bmc_show_bmcfw_version(ddata, buf);
}
static DEVICE_ATTR_RO(bmcfw_version);

static ssize_t mac_address_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *max10 = dev_get_drvdata(dev);

	return m10bmc_show_mac_address(max10, buf);
}
static DEVICE_ATTR_RO(mac_address);

static ssize_t mac_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *max10 = dev_get_drvdata(dev);

	return m10bmc_show_mac_count(max10, buf);
}
static DEVICE_ATTR_RO(mac_count);

static struct attribute *m10bmc_attrs[] = {
	&dev_attr_bmc_version.attr,
	&dev_attr_bmcfw_version.attr,
	&dev_attr_mac_address.attr,
	&dev_attr_mac_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(m10bmc);

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

static const struct m10bmc_csr spi_m10bmc_csr = {
	.base = M10BMC_SYS_BASE,
	.build_version = M10BMC_BUILD_VER,
	.fw_version = NIOS2_FW_VERSION,
	.mac_addr1 = M10BMC_MACADDR1,
	.mac_addr2 = M10BMC_MACADDR2,
	.doorbell = M10BMC_DOORBELL,
	.auth_result = M10BMC_AUTH_RESULT,
};

static int intel_m10_bmc_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *id = spi_get_device_id(spi);
	struct device *dev = &spi->dev;
	struct intel_m10bmc *m10bmc;
	int ret;

	m10bmc = devm_kzalloc(dev, sizeof(*m10bmc), GFP_KERNEL);
	if (!m10bmc)
		return -ENOMEM;

	m10bmc->dev = dev;
	m10bmc->type = (enum m10bmc_type)id->driver_data;
	m10bmc->csr = &spi_m10bmc_csr;

	m10bmc->regmap =
		devm_regmap_init_spi_avmm(spi, &intel_m10bmc_regmap_config);
	if (IS_ERR(m10bmc->regmap)) {
		ret = PTR_ERR(m10bmc->regmap);
		dev_err(dev, "Failed to allocate regmap: %d\n", ret);
		return ret;
	}

	ret = check_m10bmc_version(m10bmc);
	if (ret) {
		dev_err(m10bmc->dev, "Failed to identify m10bmc hardware\n");
		return ret;
	}

	return m10bmc_dev_init(m10bmc);
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
		.dev_groups = m10bmc_groups,
	},
	.probe = intel_m10_bmc_spi_probe,
	.id_table = m10bmc_spi_id,
};
module_spi_driver(intel_m10bmc_spi_driver);

MODULE_DESCRIPTION("MAX10 BMC SPI bus interface");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:intel-m10-bmc");
