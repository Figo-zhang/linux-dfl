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
#define to_dfl_trigger_reload(d) container_of(d, struct dfl_image_reload, trigger)

static int dfl_reload_disable_pcie_link(struct pci_dev *root, bool disable)
{
	u16 linkctl;
	int ret;

	if (!root)
		return -EINVAL;

	ret = pcie_capability_read_word(root, PCI_EXP_LNKCTL, &linkctl);
	if (ret)
		return -EINVAL;

	if (disable) {
		if (linkctl & PCI_EXP_LNKCTL_LD)
			goto out;
		linkctl |= PCI_EXP_LNKCTL_LD;
	} else {
		if (!(linkctl & PCI_EXP_LNKCTL_LD))
			goto out;
		linkctl &= ~PCI_EXP_LNKCTL_LD;
	}

	ret = pcie_capability_write_word(root, PCI_EXP_LNKCTL, linkctl);
	if (ret)
		return ret;
out:
	return 0;
}

static void dfl_reload_rescan_pci_bus(void)
{
	struct pci_bus *b = NULL;

	pci_lock_rescan_remove();
	while ((b = pci_find_next_bus(b)) != NULL)
		pci_rescan_bus(b);
	pci_unlock_rescan_remove();
}

static ssize_t available_images_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct dfl_image_reload *reload = to_dfl_image_reload(dev);
	struct dfl_image_trigger *trigger = &reload->trigger;
	ssize_t count = 0;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->available_images)
		return -EINVAL;

	mutex_lock(&dfl_priv->lock);
	count = trigger->ops->available_images(trigger, buf);
	mutex_unlock(&dfl_priv->lock);

	return count;
}

static void dfl_reload_remove_sibling_pci_dev(struct pci_dev *pcidev)
{
	struct pci_bus *bus = pcidev->bus;
	struct pci_dev *sibling, *tmp;

	if (bus) {
		list_for_each_entry_safe_reverse(sibling, tmp,
						 &bus->devices, bus_list)
			if (sibling != pcidev)
				pci_stop_and_remove_bus_device_locked(sibling);
	}
}

static ssize_t image_reload_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dfl_image_reload *reload = to_dfl_image_reload(dev);
	struct pci_dev *pcidev, *root;
	struct dfl_image_trigger *trigger = &reload->trigger;
	int ret = -EINVAL;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->image_trigger)
		return -EINVAL;

	pcidev = reload->priv;
	root = pcie_find_root_port(pcidev);
	if (!root)
		return -EINVAL;

	mutex_lock(&dfl_priv->lock);

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (reload->ops->prepare) {
		ret = reload->ops->prepare(reload);
		if (ret) {
			dev_err(&reload->dev, "prepare image reload failed\n");
			goto out;
		}
	}

	/* 3. trigger image reload */
	ret = trigger->ops->image_trigger(trigger, buf);
	if (ret) {
		dev_err(&reload->dev, "image trigger failed\n");
		goto out;
	}

	/* 4. disable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, true);
	if (ret) {
		dev_err(&reload->dev, "disable root pcie link failed\n");
		goto out;
	}

	/* 5. remove reserved devices under FP0 and PCI devices under root hub*/
	pci_stop_and_remove_bus_device_locked(root);

	/* 6. Wait for FPGA/BMC reload done. eg, 10s */
	msleep(RELOAD_TIMEOUT_MS);

	/* 7. enable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, false);
	if (ret) {
		dev_err(&reload->dev, "enable root pcie link failed\n");
		goto out;
	}

out:
	mutex_unlock(&dfl_priv->lock);

	/* 8. rescan the PCI bus*/
	dfl_reload_rescan_pci_bus();

	return ret ? : count;
}

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct dfl_image_reload *reload = to_dfl_image_reload(dev);

	if (!reload->is_registered)
		return -EINVAL;

	return sprintf(buf, "%s\n", reload->name);
}

static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(available_images);
static DEVICE_ATTR_WO(image_reload);

static struct attribute *dfl_image_reload_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_available_images.attr,
	&dev_attr_image_reload.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dfl_image_reload);

static bool dfl_match_trigger_dev(struct dfl_image_reload *reload, struct device *parent)
{
	struct pci_dev *pcidev = reload->priv;
	struct device *reload_dev = &pcidev->dev;

	/*
	 * Trigger dev is child dev of reload dev, so check
	 * the parent device of trigger dev recursively
	 */
	while (parent) {
		if (parent == reload_dev)
			return true;
		parent = parent->parent;
	}

	return false;
}

static struct dfl_image_trigger *
dfl_find_trigger(struct device *parent)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!device_is_registered(&reload->dev) ||
		    !reload->is_registered)
			continue;
		if (dfl_match_trigger_dev(reload, parent)) {
			mutex_unlock(&dfl_priv->lock);
			return &reload->trigger;
		}
	}
	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops,
				  struct device *parent, void *priv)
{
	struct dfl_image_reload *reload;
	struct dfl_image_trigger *trigger;

	if (!ops)
		return ERR_PTR(-EINVAL);

	trigger = dfl_find_trigger(parent);
	if (!trigger)
		return ERR_PTR(-EINVAL);

	reload = to_dfl_trigger_reload(trigger);

	mutex_lock(&reload->lock);
	trigger->priv = priv;
	trigger->ops = ops;
	trigger->is_registered = true;
	mutex_unlock(&reload->lock);

	return trigger;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_register);

void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger)
{
	struct dfl_image_reload *reload = to_dfl_trigger_reload(trigger);

	mutex_lock(&reload->lock);
	trigger->priv = NULL;
	trigger->ops = NULL;
	trigger->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_unregister);

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

	priv->reload_class->dev_groups = dfl_image_reload_groups;
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
