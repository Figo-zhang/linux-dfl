// SPDX-License-Identifier: GPL-2.0
/*
 * Intel DFL FPGA Image Reload Hotplug Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#include <linux/delay.h>
#include <linux/dfl.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

struct dfl_hp_image_reload_priv {
	struct list_head dev_list;
	struct mutex lock;	/* protects dev_list */
};

static struct dfl_hp_image_reload_priv *dfl_priv;

static int __init dfl_hp_image_reload_init(void)
{
	struct dfl_hp_image_reload_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->dev_list);
	dfl_priv = priv;

	return 0;
}

static void __exit dfl_hp_image_reload_exit(void)
{
	kfree(dfl_priv);
}

module_init(dfl_hp_image_reload_init);
module_exit(dfl_hp_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Hotplug Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
