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
#include <linux/delay.h>
#include "dfl-image-reload.h"

struct dfl_image_reload_priv {
	struct mutex lock; /* protect data structure contents */
	struct list_head dev_list;
};

static struct dfl_image_reload_priv *dfl_priv;

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
	struct fpga_manager *mgr = to_fpga_manager(dev);
	struct dfl_image_reload *reload = mgr->priv;
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
	struct fpga_manager *mgr = to_fpga_manager(dev);
	struct dfl_image_reload *reload = mgr->priv;
	struct dfl_image_trigger *trigger = &reload->trigger;
	enum fpga_sec_type type = trigger->type;
	struct pci_dev *pcidev, *root;
	int ret = -EINVAL;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->image_trigger)
		return -EINVAL;

	pcidev = reload->priv;
	if (!pcidev)
		return -EINVAL;

	if (type == N6000BMC_SEC)
		root = pcidev;
	else {
		root = pcie_find_root_port(pcidev);
		if (!root)
			return -EINVAL;
	}

	mutex_lock(&dfl_priv->lock);

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);
	printk("%s 1 done\n", __func__);

	/* 2. remove all non-reserved devices */
	if (mgr->mops->reload_prepare) {
		ret = mgr->mops->reload_prepare(mgr);
		if (ret) {
			dev_err(&mgr->dev, "prepare image reload failed\n");
			goto out;
		}
	}
	printk("%s 2 done\n", __func__);

#if 1
	/* 3. trigger image reload */
	ret = trigger->ops->image_trigger(trigger, buf);
	if (ret) {
		dev_err(&mgr->dev, "image trigger failed\n");
		goto out;
	}
#endif
	printk("%s 3 done\n", __func__);

	/* 4. disable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, true);
	if (ret) {
		dev_err(&mgr->dev, "disable root pcie link failed\n");
		goto out;
	}
	printk("%s 4 done\n", __func__);

	/* 5. remove reserved devices under FP0 and PCI devices under root hub*/
	pci_stop_and_remove_bus_device_locked(root);
	printk("%s 5 done\n", __func__);

	/* 6. Wait for FPGA/BMC reload done. eg, 10s */
	msleep(RELOAD_TIMEOUT_MS);

	/* 7. enable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, false);
	if (ret) {
		dev_err(&mgr->dev, "enable root pcie link failed\n");
		goto out;
	}
	printk("%s 7 done\n", __func__);

out:
	mutex_unlock(&dfl_priv->lock);

	/* 8. rescan the PCI bus*/
	dfl_reload_rescan_pci_bus();

	return ret ? : count;
}

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct fpga_manager *mgr = to_fpga_manager(dev);
	struct dfl_image_reload *reload = mgr->priv;

	if (!reload->is_registered)
		return -EINVAL;

	return sprintf(buf, "%s\n", mgr->name);
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

static const struct attribute_group dfl_reload_attr_group = {
	.name = "dfl_reload",
	.attrs = dfl_image_reload_attrs,
};

const struct attribute_group *dfl_reload_attr_groups[] = {
	&dfl_reload_attr_group,
	NULL,
};
EXPORT_SYMBOL_GPL(dfl_reload_attr_groups);

static bool dfl_match_trigger_dev(struct dfl_image_reload *reload, struct device *parent)
{
	struct pci_dev *pcidev = reload->priv;
	struct device *reload_dev = &pcidev->dev;

	/*
	 * Trigger device (security dev) is a subordinate device under
	 * reload device (pci dev), so check if the parent device of
	 * trigger device recursively
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
		if (!reload->is_registered)
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
				  struct device *parent, enum fpga_sec_type type,
				  void *priv)
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
	trigger->type = type;
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
	trigger->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_unregister);

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

	printk("pci: %04x:%02x:%02x.%d", pci_domain_nr(pcidev->bus),
                                        pcidev->bus->number, PCI_SLOT(pcidev->devfn),
                                        PCI_FUNC(pcidev->devfn));

	root = pcie_find_root_port(pcidev);
	if (!root) {
		printk("%s canot find root dev\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	 printk("root: %04x:%02x:%02x.%d", pci_domain_nr(root->bus),
                                        root->bus->number, PCI_SLOT(root->devfn),
                                        PCI_FUNC(root->devfn));

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
