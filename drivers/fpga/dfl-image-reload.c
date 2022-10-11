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

static void dfl_add_reload_dev(struct dfl_image_reload_priv *priv, struct dfl_image_reload *reload)
{
	mutex_lock(&priv->lock);
	get_device(&reload->dev);
	list_add(&reload->node, &priv->dev_list);
	mutex_unlock(&priv->lock);
}

static struct dfl_image_reload *
dfl_create_reload_dev(struct device *parent)
{
	static struct dfl_image_reload *reload;
	int ret;

	reload = kzalloc(sizeof(*reload), GFP_KERNEL);
	if (!reload)
		return ERR_PTR(-ENOMEM);

	ret = xa_alloc(&dfl_image_reload_xa, &reload->dev.id,
		       reload, DFL_IMAGE_RELOAD_XA_LIMIT, GFP_KERNEL);
	if (ret)
		goto error_kfree;

	reload->dev.class = dfl_priv->reload_class;
	reload->dev.parent = parent;

	ret = dev_set_name(&reload->dev, "dfl_reload%d", reload->dev.id);
	if (ret) {
		dev_err(&reload->dev, "Failed to set device name: dfl_reload%d\n",
			reload->dev.id);
		goto error_device;
	}

	ret = device_register(&reload->dev);
	if (ret) {
		put_device(&reload->dev);
		goto error_device;
	}

	mutex_init(&reload->lock);
	dfl_add_reload_dev(dfl_priv, reload);

	return reload;

error_device:
	xa_erase(&dfl_image_reload_xa, reload->dev.id);
error_kfree:
	kfree(reload);
	return ERR_PTR(ret);
}

static struct dfl_image_reload *
dfl_find_exist_reload(struct pci_dev *pcidev, const struct dfl_image_reload_ops *ops)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!device_is_registered(&reload->dev))
			continue;
		if (!reload->is_registered)
			continue;
		if (reload->priv == pcidev && reload->ops == ops) {
			mutex_unlock(&dfl_priv->lock);
			return reload;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static struct dfl_image_reload *dfl_find_free_reload(void)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!device_is_registered(&reload->dev))
			continue;
		if (!reload->is_registered) {
			mutex_unlock(&dfl_priv->lock);
			return reload;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static void dfl_image_reload_remove_devs(void)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		list_del(&reload->node);
		put_device(&reload->dev);

		if (device_is_registered(&reload->dev))
			device_unregister(&reload->dev);

		kfree(reload);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct dfl_image_reload_ops *ops, void *priv)
{
	struct pci_dev *pcidev, *root;
	struct dfl_image_reload *reload;

	if (!ops || !priv)
		return ERR_PTR(-EINVAL);

	pcidev = (struct pci_dev *)priv;

	root = pcie_find_root_port(pcidev);
	if (!root)
		return ERR_PTR(-EINVAL);

	reload = dfl_find_exist_reload(pcidev, ops);
	if (reload)
		return reload;

	reload = dfl_find_free_reload();
	if (!reload) {
		reload = dfl_create_reload_dev(root->dev.parent);
		if (!reload)
			return ERR_PTR(-ENODEV);
	}

	mutex_lock(&reload->lock);
	reload->priv = priv;
	reload->ops = ops;
	reload->name = name;
	reload->is_registered = true;
	mutex_unlock(&reload->lock);

	return reload;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_register);

void dfl_image_reload_dev_unregister(struct dfl_image_reload *reload)
{
	mutex_lock(&reload->lock);
	reload->priv = NULL;
	reload->ops = NULL;
	reload->name = NULL;
	reload->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_unregister);

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
	dfl_image_reload_remove_devs();
	class_destroy(dfl_priv->reload_class);

	kfree(dfl_priv);
}

module_init(dfl_image_reload_init);
module_exit(dfl_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
