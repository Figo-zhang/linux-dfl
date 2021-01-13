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
	struct m10bmc_dev mdev;
};

static void pmci_write_fifo(struct pmci_device *pmci,
		unsigned int reg, void *buf, size_t val_count)
{
	int i;
	u32 val;
	u32 *p = buf;

	for (i = 0; i < val_count; i++) {
		val = *p++;
		writel(val, pmci->base + reg + i * 4);
	}
}

static void pmci_read_fifo(struct pmci_device *pmci,
		unsigned int reg, void *buf, size_t val_count)
{
	int i;
	u32 val;
	u32 *p = buf;

	for (i = 0; i < val_count; i++) {
		val = readl(pmci->base + reg + i * 4);
		*p++ = val;
	}
}

static u32
pmci_get_write_space(struct pmci_device *pmci, u32 size)
{
	u32 count, val;
	int ret;

	ret = read_poll_timeout(readl, val,
			FIELD_GET(PMCI_FLASH_FIFO_SPACE, val) != 0,
			PMCI_FLASH_INT_US, PMCI_FLASH_TIMEOUT_US,
			false, pmci->base + PMCI_FLASH_CTRL);
	if (ret == -ETIMEDOUT)
		return 0;

	count = FIELD_GET(PMCI_FLASH_FIFO_SPACE, val) * 4;

	return (size > count) ? count : size;
}

static int
pmci_flash_bulk_write(struct intel_m10bmc *m10bmc, void *buf, u32 size)
{
	struct m10bmc_dev *mdev = container_of(m10bmc, struct m10bmc_dev, m10bmc);
	struct pmci_device *pmci = container_of(mdev, struct pmci_device, mdev);
	u32 blk_size, n_offset = 0;

	while (size) {
		blk_size = pmci_get_write_space(pmci, size);
		if (blk_size == 0) {
			dev_err(pmci->dev, "get FIFO available size fail\n");
			return -EIO;
		}
		size -= blk_size;
		pmci_write_fifo(pmci, PMCI_FLASH_FIFO,
				buf + n_offset,
			blk_size / 4);
		n_offset += blk_size;
	}

	return 0;
}

static int
pmci_set_flash_host_mux(struct pmci_device *pmci, bool request)
{
	int ret;
	u32 ctrl;

	ret = regmap_update_bits(pmci->mdev.regmap, PMCI_M10BMC_FLASH_CTRL,
			FLASH_HOST_REQUEST,
			request);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(pmci->mdev.regmap,
			PMCI_M10BMC_FLASH_CTRL,
			ctrl,
			request ? (get_flash_mux(ctrl) == FLASH_MUX_HOST) :
			(get_flash_mux(ctrl) != FLASH_MUX_HOST),
			PMCI_FLASH_INT_US,
			PMCI_FLASH_TIMEOUT_US);
}

static int
pmci_flash_bulk_read(struct intel_m10bmc *m10bmc, void *buf,
		u32 addr, u32 size)
{
	struct m10bmc_dev *mdev = container_of(m10bmc, struct m10bmc_dev, m10bmc);
	struct pmci_device *pmci = container_of(mdev, struct pmci_device, mdev);
	u32 blk_size, offset = 0, val;
	int ret;

	if (!IS_ALIGNED(addr, 4))
		return -EINVAL;

	ret = pmci_set_flash_host_mux(pmci, true);
	if (ret)
		return -EIO;

	while (size) {
		blk_size = min_t(u32, size, PMCI_READ_BLOCK_SIZE);

		writel(addr + offset, pmci->base + PMCI_FLASH_ADDR);

		writel(FIELD_PREP(PMCI_FLASH_READ_COUNT, blk_size/4)
				| PMCI_FLASH_RD_MODE,
			pmci->base + PMCI_FLASH_CTRL);

		ret = readl_poll_timeout((pmci->base + PMCI_FLASH_CTRL), val,
				 !(val & PMCI_FLASH_BUSY),
				 PMCI_FLASH_INT_US, PMCI_FLASH_TIMEOUT_US);
		if (ret) {
			dev_err(pmci->dev, "%s timed out on reading flash 0x%xn",
					__func__, val);
			goto out;
		}

		pmci_read_fifo(pmci, PMCI_FLASH_FIFO, buf, blk_size / 4);
		size -= blk_size;
		offset += blk_size;
	}

	writel(0, pmci->base + PMCI_FLASH_CTRL);
	ret = pmci_set_flash_host_mux(pmci, false);
	if (ret)
		return -EIO;

out:
	return ret;
}

static const struct fpga_flash_ops pmci_flash_ops = {
	.write_blk = pmci_flash_bulk_write,
	.read_blk = pmci_flash_bulk_read,
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

static int pmci_probe(struct dfl_device *ddev)
{
	struct device *dev = &ddev->dev;
	struct pmci_device *pmci;

	pmci = devm_kzalloc(dev, sizeof(*pmci), GFP_KERNEL);
	if (!pmci)
		return -ENOMEM;

	pmci->mdev.dev = pmci->dev = dev;
	pmci->mdev.type = M10_PMCI;

	pmci->base = devm_ioremap_resource(dev, &ddev->mmio_res);
	if (IS_ERR(pmci->base))
		return PTR_ERR(pmci->base);

	pmci->mdev.regmap = devm_regmap_init_indirect_register(dev,
			pmci->base + PMCI_SPI_BASE_OFF, &pmci_max10_cfg);
	if (IS_ERR(pmci->mdev.regmap))
		return PTR_ERR(pmci->mdev.regmap);

	pmci->mdev.m10bmc.flash_ops = &pmci_flash_ops;

	return m10bmc_dev_init(&pmci->mdev);
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
	},
	.id_table = pmci_ids,
	.probe   = pmci_probe,
};

module_dfl_driver(pmci_driver);

MODULE_DESCRIPTION("MAX10 BMC PMCI-based interface");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
