// SPDX-License-Identifier: GPL-2.0
/*
 * Intel DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2022 Intel Corporation. All rights reserved.
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
	struct fpga_card *card = to_fpga_card(dev);
	struct dfl_image_reload *reload = card->priv;
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
	struct fpga_card *card = to_fpga_card(dev);
	struct dfl_image_reload *reload = card->priv;
	struct dfl_image_trigger *trigger = &reload->trigger;
	struct pci_dev *pcidev, *remove_port;
	int ret = -EINVAL;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->image_trigger)
		return -EINVAL;

	pcidev = reload->priv;
	if (!pcidev)
		return -EINVAL;

	remove_port = pcie_find_root_port(pcidev);
	if (!remove_port)
		return -EINVAL;

	ret = fpga_card_lock(card);
	if (ret)
		return ret;

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	ret = fpga_card_prepare_image_reload(card);
	if (ret) {
		dev_err(&card->dev, "prepare image reload failed\n");
		goto out;
	}

	/* 3. trigger image reload */
	ret = trigger->ops->image_trigger(trigger, buf);
	if (ret) {
		dev_err(&card->dev, "image trigger failed\n");
		goto out;
	}

	/* 4. disable pci link of remove port */
	ret = dfl_reload_disable_pcie_link(remove_port, true);
	if (ret) {
		dev_err(&card->dev, "disable pcie link of remove port failed\n");
		goto out;
	}

	/* 5. remove reserved devices under PF0 and PCI devices under remove port*/
	pci_stop_and_remove_bus_device_locked(remove_port);

	/* 6. Wait for FPGA/BMC reload done */
	ssleep(trigger->wait_time);

	/* 7. enable pci link of remove port */
	ret = dfl_reload_disable_pcie_link(remove_port, false);
	if (ret) {
		dev_err(&card->dev, "enable pcie link of remove port failed\n");
		goto out;
	}

out:
	fpga_card_unlock(card);

	/* 8. rescan the PCI bus*/
	dfl_reload_rescan_pci_bus();

	return ret ? : count;
}

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct fpga_card *card = to_fpga_card(dev);
	struct dfl_image_reload *reload = card->priv;

	if (!reload->is_registered)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", card->name);
}

static ssize_t reload_wait_secs_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct fpga_card *card = to_fpga_card(dev);
	struct dfl_image_reload *reload = card->priv;
	struct dfl_image_trigger *trigger = &reload->trigger;
	unsigned long val;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	mutex_lock(&reload->lock);
	trigger->wait_time = val;
	mutex_unlock(&reload->lock);

	return count;
}

static ssize_t reload_wait_secs_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct fpga_card *card = to_fpga_card(dev);
	struct dfl_image_reload *reload = card->priv;
	struct dfl_image_trigger *trigger = &reload->trigger;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	return sysfs_emit(buf, "%u\n", trigger->wait_time);
}

static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(available_images);
static DEVICE_ATTR_WO(image_reload);
static DEVICE_ATTR_RW(reload_wait_secs);

static struct attribute *dfl_image_reload_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_available_images.attr,
	&dev_attr_image_reload.attr,
	&dev_attr_reload_wait_secs.attr,
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
	trigger->parent = parent;
	trigger->ops = ops;
	trigger->wait_time = RELOAD_DEFAULT_WAIT_SECS;
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
			  const struct fpga_card_ops *ops, void *priv)
{
	static struct dfl_image_reload *reload;
	int ret;

	reload = kzalloc(sizeof(*reload), GFP_KERNEL);
	if (!reload)
		return ERR_PTR(-ENOMEM);

	mutex_init(&reload->lock);

	reload->card = fpga_card_register(parent, name, ops, reload);
	if (!reload->card)
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
dfl_find_exist_reload(struct pci_dev *pcidev, const struct fpga_card_ops *ops)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!reload->is_registered)
			continue;
		if (reload->card->priv == pcidev && reload->card->mops == ops) {
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
	struct fpga_card *card;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		card = reload->card;

		/*
		 * card->mops have been released because the dfl-pci module
		 * remove firstly
		 */
		device_unregister(&card->dev);
		list_del(&reload->node);
		kfree(reload);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct fpga_card_ops *ops, void *priv)
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
	reload->card->mops = ops;
	reload->card->name = name;
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
