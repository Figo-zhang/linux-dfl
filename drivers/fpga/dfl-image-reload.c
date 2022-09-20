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

static struct class *dfl_image_reload_class;
static struct dfl_image_reload *dfl_reload;

#define to_dfl_image_reload(d) container_of(d, struct dfl_image_reload, dev)

struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops, void *priv)
{
	struct dfl_image_trigger *trigger = &dfl_reload->trigger;

	if (!ops) {
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	trigger->priv = priv;
	trigger->ops = ops;

	return trigger;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_register);

void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger)
{
	trigger->priv = NULL;
	trigger->ops = NULL;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_unregister);

struct dfl_image_reload *
dfl_image_reload_dev_register(const struct dfl_image_reload_ops *ops, void *priv)
{
	if (!ops) {
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	dfl_reload->priv = priv;
	dfl_reload->ops = ops;

	return dfl_reload;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_register);

void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload)
{
	dfl_reload->priv = NULL;
	dfl_reload->ops = NULL;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_unregister);

static void dfl_image_reload_dev_release(struct device *dev)
{
	struct dfl_image_reload *dfl_reload = to_dfl_image_reload(dev);

	xa_erase(&dfl_image_reload_xa, dfl_reload->dev.id);
}

static int __init dfl_image_reload_init(void)
{
	int ret;

	dfl_image_reload_class = class_create(THIS_MODULE, "dfl_image_reload");
	if (IS_ERR(dfl_image_reload_class))
		return PTR_ERR(dfl_image_reload_class);

	dfl_image_reload_class->dev_release = dfl_image_reload_dev_release;

	dfl_reload = kzalloc(sizeof(*dfl_reload), GFP_KERNEL);
	if (!dfl_reload) {
		ret = -ENOMEM;
		goto free_class;
	}

	ret = xa_alloc(&dfl_image_reload_xa, &dfl_reload->dev.id,
		       dfl_reload, DFL_IMAGE_RELOAD_XA_LIMIT, GFP_KERNEL);
	if (ret)
		goto error_kfree;

	dfl_reload->dev.class = dfl_image_reload_class;
	dfl_reload->dev.parent = NULL;

	ret = dev_set_name(&dfl_reload->dev, "dfl_reload%d", dfl_reload->dev.id);
	if (ret) {
		dev_err(&dfl_reload->dev, "Failed to set device name: dfl_reload%d\n",
			dfl_reload->dev.id);
		goto error_device;
	}

	ret = device_register(&dfl_reload->dev);
	if (ret) {
		put_device(&dfl_reload->dev);
		goto error_device;
	}

	mutex_init(&dfl_reload->lock);

	return 0;

error_device:
	xa_erase(&dfl_image_reload_xa, dfl_reload->dev.id);
error_kfree:
	kfree(dfl_reload);
free_class:
	class_destroy(dfl_image_reload_class);

	return ret;
}

static void __exit dfl_image_reload_exit(void)
{
	device_unregister(&dfl_reload->dev);

	class_destroy(dfl_image_reload_class);

	kfree(dfl_reload);
}

module_init(dfl_image_reload_init);
module_exit(dfl_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
