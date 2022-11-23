// SPDX-License-Identifier: GPL-2.0
/*
 * Intel DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#include <linux/delay.h>
#include <linux/dfl.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uaccess.h>

#include "dfl-image-reload.h"

extern int pci_hp_add_bridge(struct pci_dev *dev);
//extern bool pcie_wait_for_link(struct pci_dev *pdev, bool active);

struct dfl_image_reload_priv {
	struct list_head dev_list;
	struct mutex lock; /* protect data structure contents */
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

static ssize_t dfl_hotplug_available_images(struct hotplug_slot *slot, char *buf)
{
	struct dfl_image_reload *reload = to_dfl_reload(slot);
	struct dfl_image_trigger *trigger = &reload->trigger;
	ssize_t count;

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

static void dfl_reload_remove_hotplug_slot(struct pci_dev *hotplug_slot)
{
	struct pci_dev *dev, *temp;
	struct pci_bus *parent = hotplug_slot->subordinate;
	u16 command;

	pci_lock_rescan_remove();
	list_for_each_entry_safe_reverse(dev, temp, &parent->devices,
					 bus_list) {
		printk("%s: remove === %s\n", __func__, dev_name(&dev->dev));
		pci_dev_get(dev);
		pci_stop_and_remove_bus_device(dev);

		pci_read_config_word(dev, PCI_COMMAND, &command);
		command &= ~(PCI_COMMAND_MASTER | PCI_COMMAND_SERR);
		command |= PCI_COMMAND_INTX_DISABLE;
		pci_write_config_word(dev, PCI_COMMAND, command);
		pci_dev_put(dev);
	}

	pci_unlock_rescan_remove();
}

static int dfl_configure_slot(struct pci_dev *hotplug_slot)
{
	struct pci_dev *dev;
	struct pci_bus *parent = hotplug_slot->subordinate;
	int num, ret = 0;

		pci_lock_rescan_remove();

	dev = pci_get_slot(parent, PCI_DEVFN(0, 0));
	if (dev) {
		/*
		 * The device is already there. Either configured by the
		 * boot firmware or a previous hotplug event.
		 */
		printk("Device %s already exists at %04x:%02x:00, skipping hot-add\n",
			 pci_name(dev), pci_domain_nr(parent), parent->number);
		pci_dev_put(dev);
		ret = -EEXIST;
		goto out;
	}

	num = pci_scan_slot(parent, PCI_DEVFN(0, 0));
	if (num == 0) {
		printk("No new device found\n");
		ret = -ENODEV;
		goto out;
	}

	for_each_pci_bridge(dev, parent)
		pci_hp_add_bridge(dev);

	pci_assign_unassigned_bridge_resources(hotplug_slot);
	pcie_bus_configure_settings(parent);
	pci_bus_add_devices(parent);

 out:
	pci_unlock_rescan_remove();
	return ret;
}

static int dfl_hotplug_image_reload(struct hotplug_slot *slot, const char *buf)
{
	struct dfl_image_reload *reload = to_dfl_reload(slot);
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

	reload->state = IMAGE_RELOAD_RELOADING;

	mutex_lock(&dfl_priv->lock);

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (reload->ops->reload_prepare) {
		ret = reload->ops->reload_prepare(reload);
		if (ret) {
			dev_err(&remove_port->dev, "prepare image reload failed\n");
			goto out;
		}
	}

	/* 3. trigger image reload */
	ret = trigger->ops->image_trigger(trigger, buf);
	if (ret) {
		dev_err(&remove_port->dev, "image trigger failed\n");
		goto out;
	}

	/* 4. disable pci link of remove port */
	ret = dfl_reload_disable_pcie_link(remove_port, true);
	if (ret) {
		dev_err(&remove_port->dev, "disable pcie link of remove port failed\n");
		goto out;
	}

	printk("5 --------------------------\n");
	/* 5. remove reserved devices under PF0 and PCI devices under remove port*/
	//pci_stop_and_remove_bus_device_locked(remove_port);
	dfl_reload_remove_hotplug_slot(remove_port);
	//pci_stop_root_bus(remove_port->subordinate);

	/* 6. Wait for FPGA/BMC reload done */
	ssleep(10);

	pcie_capability_write_word(remove_port, PCI_EXP_SLTCTL, PCI_EXP_SLTCTL_PCC);
	ssleep(1);

	/* 7. enable pci link of remove port */
	ret = dfl_reload_disable_pcie_link(remove_port, false);
	if (ret) {
		dev_err(&remove_port->dev, "enable pcie link of remove port failed\n");
		goto out;
	}

	msleep(1000);

out:
	mutex_unlock(&dfl_priv->lock);

	/* 8. rescan the PCI bus*/
	dfl_reload_rescan_pci_bus();
	//dfl_configure_slot(remove_port);
	
	reload->state = IMAGE_RELOAD_DONE;

	return ret;
}

static const struct hotplug_slot_ops dfl_hotplug_slot_ops = {
	.available_images	= dfl_hotplug_available_images,
	.image_reload	        = dfl_hotplug_image_reload
};

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

static int
dfl_create_new_reload_dev(struct dfl_image_reload *reload, struct pci_dev *pcidev, 
		const char * name, const struct dfl_image_reload_ops *ops, void *priv)
{
	u32 slot_cap;
	char slot_name[SLOT_NAME_SIZE];
	int ret;

	pcie_capability_read_dword(pcidev, PCI_EXP_SLTCAP, &slot_cap);
	snprintf(slot_name, SLOT_NAME_SIZE, "%u", (slot_cap & PCI_EXP_SLTCAP_PSN) >> 19);

	reload->slot.ops = &dfl_hotplug_slot_ops;

	/* Register PCI slot */
	ret = pci_hp_register(&reload->slot, pcidev->subordinate, PCI_SLOT(pcidev->devfn), slot_name); 
	if (ret) {
		pr_err("pci_hp_register failed with error %d\n", ret);
		return ret;
	}

	printk("%s ===== \n", __func__);

	pr_info("Slot [%s] registered\n", hotplug_slot_name(&reload->slot));

	mutex_init(&reload->lock);

	mutex_lock(&reload->lock);
	reload->hotplug_dev = pcidev;
	reload->ops = ops;
	reload->name = name;
	reload->priv = priv;
	reload->is_registered = true;
	reload->state = IMAGE_RELOAD_UNKNOWN;
	mutex_unlock(&reload->lock);

	dfl_add_reload_dev(dfl_priv, reload);

	return 0;

}

static struct dfl_image_reload *
dfl_find_exist_reload(struct pci_dev *pcidev, const struct dfl_image_reload_ops *ops)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (!reload->is_registered)
			continue;
		if (reload->priv == pcidev && reload->ops == ops) {
			mutex_unlock(&dfl_priv->lock);
			printk("%s ===== \n", __func__);
			return reload;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static struct dfl_image_reload *dfl_find_free_reload(struct pci_dev *hotplug_dev)
{
	struct dfl_image_reload *reload, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(reload, tmp, &dfl_priv->dev_list, node) {
		if (reload->is_registered)
			continue;

		/* reclaim unused reload */
		if (reload->hotplug_dev == hotplug_dev) {
			printk("%s reuse it %s \n", __func__, reload->name);
			mutex_unlock(&dfl_priv->lock);
			return reload;
		}

		/* free unused reload */
		if (!reload->is_registered && reload->state == IMAGE_RELOAD_DONE) {
			list_del(&reload->node);
			printk("%s free it %s \n", __func__, reload->name);
			pci_hp_deregister(&reload->slot);
			kfree(reload);
			continue;
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
		printk("%s ===== %s \n", __func__, reload->name);
		pci_hp_deregister(&reload->slot);
		kfree(reload);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct dfl_image_reload_ops *ops, void *priv)
{
	struct pci_dev *pcidev, *hotplug_dev;
	struct dfl_image_reload *reload;
	int ret;

	if (!ops || !priv)
		return ERR_PTR(-EINVAL);

	pcidev = (struct pci_dev *)priv;

	dev_dbg(&pcidev->dev, "registering pci: %04x:%02x:%02x.%d to reload driver\n",
			pci_domain_nr(pcidev->bus), pcidev->bus->number,
			PCI_SLOT(pcidev->devfn), PCI_FUNC(pcidev->devfn));

	/* For N3000, the hotplug port was on the root port of PF0 */
	hotplug_dev = pcie_find_root_port(pcidev);
	if (!hotplug_dev)
		return ERR_PTR(-EINVAL);

	printk("%s hotplug_dev %lx pcidev %lx\n",__func__, (unsigned long)hotplug_dev, (unsigned long)pcidev);

	dev_dbg(&pcidev->dev, "hotplug slot: %04x:%02x:%02x\n",
			pci_domain_nr(hotplug_dev->bus), hotplug_dev->bus->number,
			PCI_SLOT(hotplug_dev->devfn));

	/* find exist matched reload dev */
	reload = dfl_find_exist_reload(pcidev, ops);
	if (reload)
		return reload;

	/* can it reuse the free reload dev? */
	reload = dfl_find_free_reload(hotplug_dev);
	if (reload) {
		printk("%s can reuse\n", __func__);
		goto reuse;
	}

	printk("%s create a new one\n", __func__);
	reload = kzalloc(sizeof(*reload), GFP_KERNEL);
	if (!reload)
		return ERR_PTR(-ENOMEM);

	/* create new reload dev */
	ret = dfl_create_new_reload_dev(reload, hotplug_dev, name, ops, priv);
	if (ret) {
		kfree(reload);
		return ERR_PTR(ret);
	}

reuse:
	printk("%s reuse reload\n", __func__);
	mutex_lock(&reload->lock);
	reload->ops = ops;
	reload->name = name;
	reload->priv = priv;
	reload->is_registered = true;
	reload->state = IMAGE_RELOAD_UNKNOWN;
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
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
