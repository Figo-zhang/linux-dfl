// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA PCI Hotplug Manager Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

struct fpgahp_priv {
	struct list_head dev_list;
	struct mutex lock;	/* protects dev_list */
};

static struct fpgahp_priv *fpgahp_priv;

static int __init fpgahp_init(void)
{
	struct fpgahp_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->dev_list);
	fpgahp_priv = priv;

	return 0;
}

static void __exit fpgahp_exit(void)
{
	kfree(fpgahp_priv);
}

module_init(fpgahp_init);
module_exit(fpgahp_exit);

MODULE_DESCRIPTION("FPGA PCI Hotplug Manager Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
