// SPDX-License-Identifier: GPL-2.0
/*
 * Intel DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2019-2022 Intel Corporation. All rights reserved.
 *
 */
#include <linux/dfl.h>
#include <linux/pci.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include "dfl.h"
#include "dfl-image-reload.h"

#define DFL_IMAGE_RELOAD_XA_LIMIT  XA_LIMIT(0, INT_MAX)
static DEFINE_XARRAY_ALLOC(dfl_image_reload_xa);

struct dfl_image_reload_priv {
	struct mutex lock; /* protect data structure contents */
	struct list_head dev_list;
	struct class *reload_class;
};

static struct dfl_image_reload_priv *dfl_priv;

#define to_dfl_image_reload(d) container_of(d, struct dfl_image_reload, dev)

static void dfl_image_reload_dev_release(struct device *dev)
{
	struct dfl_image_reload *reload = to_dfl_image_reload(dev);

	xa_erase(&dfl_image_reload_xa, reload->dev.id);
}

static int __init dfl_image_reload_init(void)
{
	struct dfl_image_reload_priv *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reload_class = class_create(THIS_MODULE, "dfl_image_reload");
	if (!priv->reload_class) {
		ret = -EINVAL;
		goto free;
	}

	priv->reload_class->dev_release = dfl_image_reload_dev_release;

	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->dev_list);
	dfl_priv = priv;

	return 0;

free:
	kfree(priv);
	return ret;
}

static void __exit dfl_image_reload_exit(void)
{
	class_destroy(dfl_priv->reload_class);

	kfree(dfl_priv);
}

module_init(dfl_image_reload_init);
module_exit(dfl_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
