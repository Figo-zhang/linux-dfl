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
#include "dfl-image-reload.h"

struct dfl_image_reload_priv {
	struct mutex lock; /* protect data structure contents */
	struct list_head dev_list;
};

static struct dfl_image_reload_priv *dfl_priv;

static void dfl_add_reload_dev(struct dfl_image_reload_priv *priv, struct dfl_image_reload *reload)
{
	mutex_lock(&priv->lock);
	list_add(&reload->node, &priv->dev_list);
	mutex_unlock(&priv->lock);
}

static struct dfl_image_reload *
dfl_create_new_reload_dev(struct device *parent, const char *name,
			  const struct fpga_manager_ops *ops, void *priv)
{
	static struct dfl_image_reload *reload;
	int ret;

	reload = kzalloc(sizeof(*reload), GFP_KERNEL);
	if (!reload)
		return ERR_PTR(-ENOMEM);

	mutex_init(&reload->lock);

	reload->mgr = fpga_mgr_register(parent, name, ops, reload);
	if (!reload->mgr)
		goto error_kfree;

	mutex_lock(&reload->lock);
	reload->priv = priv;
	reload->is_registered = true;
	mutex_unlock(&reload->lock);

	dfl_add_reload_dev(dfl_priv, reload);

	return reload;

error_kfree:
	kfree(reload);
	return ERR_PTR(ret);
}

static struct dfl_image_reload *
dfl_find_exist_reload(struct pci_dev *pcidev, const struct fpga_manager_ops *ops)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!reload->is_registered)
			continue;
		if (reload->mgr->priv == pcidev && reload->mgr->mops == ops) {
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
	struct fpga_manager *mgr;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		mgr = reload->mgr;

		/*
		 * mgr->mops have been released because the dfl-pci module
		 * remove firstly
		 */
		device_unregister(&mgr->dev);
		list_del(&reload->node);
		kfree(reload);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct fpga_manager_ops *ops, void *priv)
{
	struct pci_dev *pcidev, *root;
	struct dfl_image_reload *reload;

	if (!ops || !priv)
		return ERR_PTR(-EINVAL);

	pcidev = (struct pci_dev *)priv;

	root = pcie_find_root_port(pcidev);
	if (!root)
		return ERR_PTR(-EINVAL);

	/* find exist matched reload dev */
	reload = dfl_find_exist_reload(pcidev, ops);
	if (reload)
		return reload;

	/* can it reuse the free reload dev? */
	reload = dfl_find_free_reload();
	if (reload)
		goto reuse;

	/* create new reload dev */
	reload = dfl_create_new_reload_dev(root->dev.parent, name, ops, priv);
	if (reload)
		return reload;

reuse:
	mutex_lock(&reload->lock);
	reload->mgr->mops = ops;
	reload->mgr->name = name;
	reload->priv = priv;
	reload->is_registered = true;
	mutex_unlock(&reload->lock);

	return reload;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_register);

void dfl_image_reload_dev_unregister(struct dfl_image_reload *reload)
{
	mutex_lock(&reload->lock);
	reload->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_unregister);

static int __init dfl_image_reload_init(void)
{
	struct dfl_image_reload_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->dev_list);
	dfl_priv = priv;

	return 0;
}

static void __exit dfl_image_reload_exit(void)
{
	dfl_image_reload_remove_devs();
	kfree(dfl_priv);
}

module_init(dfl_image_reload_init);
module_exit(dfl_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
