// SPDX-License-Identifier: GPL-2.0
/*
 * Core MFD support for Intel MAX 10 Board Management Controller chip
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

static struct mfd_cell pmci_bmc_subdevs[] = {
	{ .name = "intel-pmci-hwmon" },
	{ .name = "intel-pmci-secure" }
};

static struct mfd_cell m10bmc_bmc_subdevs[] = {
	{ .name = "d5005bmc-hwmon" },
	{ .name = "d5005bmc-secure" }
};

static struct resource retimer0_resources[] = {
	{M10BMC_PKVL_A_VER, M10BMC_PKVL_A_VER, "version", IORESOURCE_REG, },
};

static struct resource retimer1_resources[] = {
	{M10BMC_PKVL_B_VER, M10BMC_PKVL_B_VER, "version", IORESOURCE_REG, },
};

static struct mfd_cell m10bmc_pacn3000_subdevs[] = {
	{ .name = "n3000bmc-hwmon" },
	{
		.name = "n3000bmc-retimer",
		.num_resources = ARRAY_SIZE(retimer0_resources),
		.resources = retimer0_resources,
	},
	{
		.name = "n3000bmc-retimer",
		.num_resources = ARRAY_SIZE(retimer1_resources),
		.resources = retimer1_resources,
	},
	{ .name = "n3000bmc-secure" },
};

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

	ret = m10bmc_sys_read(ddata, M10BMC_BUILD_VER, &val);
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

	ret = m10bmc_sys_read(ddata, NIOS2_FW_VERSION, &val);
	if (ret)
		return ret;

	return sprintf(buf, "0x%x\n", val);
}
static DEVICE_ATTR_RO(bmcfw_version);

static ssize_t mac_address_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *max10 = dev_get_drvdata(dev);
	unsigned int macaddr1, macaddr2;
	int ret;

	ret = m10bmc_sys_read(max10, M10BMC_MACADDR1, &macaddr1);
	if (ret)
		return ret;

	ret = m10bmc_sys_read(max10, M10BMC_MACADDR2, &macaddr2);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n",
			  (u8)FIELD_GET(M10BMC_MAC_BYTE1, macaddr1),
			  (u8)FIELD_GET(M10BMC_MAC_BYTE2, macaddr1),
			  (u8)FIELD_GET(M10BMC_MAC_BYTE3, macaddr1),
			  (u8)FIELD_GET(M10BMC_MAC_BYTE4, macaddr1),
			  (u8)FIELD_GET(M10BMC_MAC_BYTE5, macaddr2),
			  (u8)FIELD_GET(M10BMC_MAC_BYTE6, macaddr2));
}
static DEVICE_ATTR_RO(mac_address);

static ssize_t mac_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct intel_m10bmc *max10 = dev_get_drvdata(dev);
	unsigned int macaddr2;
	int ret;

	ret = m10bmc_sys_read(max10, M10BMC_MACADDR2, &macaddr2);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n",
			  (u8)FIELD_GET(M10BMC_MAC_COUNT, macaddr2));
}
static DEVICE_ATTR_RO(mac_count);

static struct attribute *m10bmc_attrs[] = {
	&dev_attr_bmc_version.attr,
	&dev_attr_bmcfw_version.attr,
	&dev_attr_mac_address.attr,
	&dev_attr_mac_count.attr,
	NULL,
};

const struct attribute_group m10bmc_group = {
	.attrs = m10bmc_attrs,
};

int m10bmc_dev_init(struct intel_m10bmc *m10bmc)
{
	struct mfd_cell *cells;
	int ret, n_cell;

	init_rwsem(&m10bmc->bmcfw_lock);

	dev_set_drvdata(m10bmc->dev, m10bmc);

	switch (m10bmc->type) {
	case M10_N3000:
		cells = m10bmc_pacn3000_subdevs;
		n_cell = ARRAY_SIZE(m10bmc_pacn3000_subdevs);
		break;
	case M10_D5005:
		cells = m10bmc_bmc_subdevs;
		n_cell = ARRAY_SIZE(m10bmc_bmc_subdevs);
		break;
	case M10_PMCI:
		cells = pmci_bmc_subdevs;
		n_cell = ARRAY_SIZE(pmci_bmc_subdevs);
		break;
	default:
		return -ENODEV;
	}

	ret = devm_mfd_add_devices(m10bmc->dev, PLATFORM_DEVID_AUTO, cells, n_cell,
				   NULL, 0, NULL);
	if (ret)
		dev_err(m10bmc->dev, "Failed to register sub-devices: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(m10bmc_dev_init);

MODULE_DESCRIPTION("Intel MAX 10 BMC core MFD driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
