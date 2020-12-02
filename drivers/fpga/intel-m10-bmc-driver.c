// SPDX-License-Identifier: GPL-2.0
/*
 * Intel MAX 10 BMC Driver
 *
 * Copyright (C) 2020-2021 Intel Corporation. All rights reserved.
 *
 */
#include <linux/device.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

static const struct regmap_range n3000_fw_handshake_regs[] = {
	regmap_reg_range(M10BMC_TELEM_START, M10BMC_TELEM_END),
};

int m10bmc_fw_state_enter(struct intel_m10bmc *m10bmc,
			  enum m10bmc_fw_state new_state)
{
	int ret = 0;

	if (new_state == M10BMC_FW_STATE_NORMAL)
		return -EINVAL;

	down_write(&m10bmc->bmcfw_lock);

	if (m10bmc->bmcfw_state == M10BMC_FW_STATE_NORMAL)
		m10bmc->bmcfw_state = new_state;
	else if (m10bmc->bmcfw_state != new_state)
		ret = -EBUSY;

	up_write(&m10bmc->bmcfw_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(m10bmc_fw_state_enter);

void m10bmc_fw_state_exit(struct intel_m10bmc *m10bmc)
{
	down_write(&m10bmc->bmcfw_lock);

	m10bmc->bmcfw_state = M10BMC_FW_STATE_NORMAL;

	up_write(&m10bmc->bmcfw_lock);
}
EXPORT_SYMBOL_GPL(m10bmc_fw_state_exit);

static bool is_handshake_sys_reg(unsigned int offset)
{
	return regmap_reg_in_ranges(offset, n3000_fw_handshake_regs,
				    ARRAY_SIZE(n3000_fw_handshake_regs));
}

int m10bmc_sys_read(struct intel_m10bmc *m10bmc, unsigned int offset,
		    unsigned int *val)
{
	int ret;

	if (!is_handshake_sys_reg(offset))
		return m10bmc_raw_read(m10bmc, M10BMC_SYS_BASE + (offset), val);

	down_read(&m10bmc->bmcfw_lock);

	if (m10bmc->bmcfw_state == M10BMC_FW_STATE_SEC_UPDATE)
		ret = -EBUSY;
	else
		ret = m10bmc_raw_read(m10bmc, M10BMC_SYS_BASE + (offset), val);

	up_read(&m10bmc->bmcfw_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(m10bmc_sys_read);

int m10bmc_sys_update_bits(struct intel_m10bmc *m10bmc, unsigned int offset,
			   unsigned int msk, unsigned int val)
{
	int ret;

	if (!is_handshake_sys_reg(offset))
		return regmap_update_bits(m10bmc->regmap,
					  M10BMC_SYS_BASE + (offset), msk, val);

	down_read(&m10bmc->bmcfw_lock);

	if (m10bmc->bmcfw_state == M10BMC_FW_STATE_SEC_UPDATE)
		ret = -EBUSY;
	else
		ret = regmap_update_bits(m10bmc->regmap,
					 M10BMC_SYS_BASE + (offset), msk, val);

	up_read(&m10bmc->bmcfw_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(m10bmc_sys_update_bits);

static ssize_t bmc_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *ddata = dev_get_drvdata(dev);
	unsigned int val;
	int ret;
	u32 reg;

	if (M10_SPI_CARD(ddata))
		reg = M10BMC_BUILD_VER;
	else if (M10_PMCI_CARD(ddata))
		reg = PMCI_M10BMC_BUILD_VER;

	ret = m10bmc_sys_read(ddata, reg, &val);
	if (ret)
		return ret;

	return sprintf(buf, "0x%x\n", val);
}
static DEVICE_ATTR_RO(bmc_version);

static ssize_t bmcfw_version_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *ddata = dev_get_drvdata(dev);
	unsigned int val;
	int ret;
	u32 reg;

	if (M10_SPI_CARD(ddata))
		reg = NIOS2_FW_VERSION;
	else if (M10_PMCI_CARD(ddata))
		reg = PMCI_NIOS2_FW_VERSION;

	ret = m10bmc_sys_read(ddata, reg, &val);
	if (ret)
		return ret;

	return sprintf(buf, "0x%x\n", val);
}
static DEVICE_ATTR_RO(bmcfw_version);

static struct attribute *m10bmc_attrs[] = {
	&dev_attr_bmc_version.attr,
	&dev_attr_bmcfw_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(m10bmc);

static int check_m10bmc_version(struct intel_m10bmc *ddata)
{
	unsigned int v;
	int ret;
	u32 reg;

	if (M10_SPI_CARD(ddata))
		reg = M10BMC_BUILD_VER;
	else if (M10_PMCI_CARD(ddata))
		reg = PMCI_M10BMC_BUILD_VER;

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
			      M10BMC_LEGACY_SYS_BASE + reg, &v);
	if (ret)
		return -ENODEV;

	if (v != M10BMC_VER_LEGACY_INVALID) {
		dev_err(ddata->dev, "bad version M10BMC detected\n");
		return -ENODEV;
	}

	return 0;
}

static int m10bmc_probe(struct platform_device *pdev)
{
	struct intel_m10bmc *m10bmc = dev_get_drvdata(pdev->dev.parent);
	const struct platform_device_id *id = platform_get_device_id(pdev);
	enum m10bmc_type type = (enum m10bmc_type)id->driver_data;
	int ret;

	ret = check_m10bmc_version(m10bmc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to identify m10bmc hardware\n");
		return ret;
	}

	m10bmc->type = type;

	return ret;
}

static const struct platform_device_id intel_m10bmc_ids[] = {
	{
		.name = "n3000bmc-max10bmc",
		.driver_data = (unsigned long)M10_N3000,
	},
	{
		.name = "d5005bmc-max10bmc",
		.driver_data = (unsigned long)M10_D5005,
	},
	{
		.name = "ac-pmci-max10bmc",
		.driver_data = (unsigned long)M10_AC,
	},
	{ }
};

static struct platform_driver intel_m10bmc_driver = {
	.probe = m10bmc_probe,
	.driver = {
		.name = "intel-m10-bmc-driver",
		.dev_groups = m10bmc_groups,
	},
	.id_table = intel_m10bmc_ids,
};
module_platform_driver(intel_m10bmc_driver);

MODULE_DEVICE_TABLE(platform, intel_m10bmc_ids);
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Intel MAX 10 BMC driver");
MODULE_LICENSE("GPL");
