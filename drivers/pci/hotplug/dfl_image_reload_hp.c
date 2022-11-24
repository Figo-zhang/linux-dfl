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
#include <linux/fpga/dfl-image-reload.h>

#include "pciehp.h"

extern int pci_hp_add_bridge(struct pci_dev *dev);

struct dfl_image_reload_priv {
	struct list_head dev_list;
	struct mutex lock; /* protect data structure contents */
};

struct reload_hp_controller {
	struct list_head node;
	struct pcie_device *pcie;
	struct controller ctrl;
	struct pci_dev *hotplug_bridge;
	struct dfl_image_reload reload;
};

static struct dfl_image_reload_priv *dfl_priv;

#define to_dfl_trigger_reload(d) container_of(d, struct dfl_image_reload, trigger)
#define to_hpc(d) container_of(d, struct reload_hp_controller, ctrl)

static ssize_t dfl_hotplug_available_images(struct hotplug_slot *slot, char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct reload_hp_controller *hpc = to_hpc(ctrl);
	struct dfl_image_reload *reload = &hpc->reload;
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

static void set_slot_off(struct controller *ctrl)
{
	/*
	 * Turn off slot
	 */
	if (POWER_CTRL(ctrl)) {
		pciehp_power_off_slot(ctrl);

		/*
		 * After turning power off, we must wait for at least 1 second
		 * before taking any action that relies on power having been
		 * removed from the slot/adapter.
		 */
		msleep(1000);
	}
}

static int dfl_hotplug_rescan_slot(struct controller *ctrl)
{
	int retval = 0;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	if (POWER_CTRL(ctrl)) {
		printk("%s want to power on slot\n", __func__);
		/* Power on slot */
		retval = pciehp_power_on_slot(ctrl);
		if (retval)
			return retval;

		msleep(1000);
	}

	/* Check link training status */
	//retval = pciehp_check_link_status(ctrl);
	//if (retval)
	//	goto err_exit;

	/* Check for a power fault */
	if (ctrl->power_fault_detected || pciehp_query_power_fault(ctrl)) {
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		retval = -EIO;
		goto err_exit;
	}

	retval = pciehp_configure_device(ctrl);
	if (retval) {
		if (retval != -EEXIST) {
			ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
				 pci_domain_nr(parent), parent->number);
			goto err_exit;
		}
	}

	return 0;

err_exit:
	set_slot_off(ctrl);
	return retval;
}

static void cleanup_slot(struct controller *ctrl)
{
	struct hotplug_slot *hotplug_slot = &ctrl->hotplug_slot;

	pci_hp_destroy(hotplug_slot);
	kfree(hotplug_slot->ops);
}

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
	struct controller *ctrl = to_ctrl(slot);
	struct reload_hp_controller *hpc = to_hpc(ctrl);
	struct dfl_image_reload *reload = &hpc->reload;
	struct dfl_image_trigger *trigger = &reload->trigger;
	struct pci_dev *hotplug_bridge = hpc->hotplug_bridge;
	struct pci_dev *pcidev;
	int ret = -EINVAL;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->image_trigger)
		return -EINVAL;

	pcidev = reload->priv;
	if (!pcidev)
		return -EINVAL;

	reload->state = IMAGE_RELOAD_RELOADING;

	mutex_lock(&dfl_priv->lock);

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (reload->ops->reload_prepare) {
		ret = reload->ops->reload_prepare(reload);
		if (ret) {
			ctrl_err(ctrl, "prepare image reload failed\n");
			goto out;
		}
	}

	/* 3. trigger image reload */
	ret = trigger->ops->image_trigger(trigger, buf);
	if (ret) {
		ctrl_err(ctrl, "image trigger failed\n");
		goto out;
	}

	dfl_reload_disable_pcie_link(hotplug_bridge, true);

	/* 4. remove PCI devices below a hotplug bridge */
	//pciehp_unconfigure_device(ctrl, true);
	dfl_reload_remove_hotplug_slot(hotplug_bridge);

	/* 6. Wait for FPGA/BMC reload done */
	ssleep(10);

	/* 7. turn off slot */
	//set_slot_off(ctrl);
	pcie_capability_write_word(hotplug_bridge, PCI_EXP_SLTCTL, PCI_EXP_SLTCTL_PCC);
        ssleep(1);

out:
	mutex_unlock(&dfl_priv->lock);

	//dfl_hotplug_rescan_slot(ctrl);
	pcie_capability_write_word(hotplug_bridge, PCI_EXP_SLTCTL, PCI_EXP_SLTCTL_PWR_ON);
	dfl_reload_disable_pcie_link(hotplug_bridge, false);
	msleep(1000);
	dfl_configure_slot(hotplug_bridge);
	//dfl_reload_rescan_pci_bus();
	
	reload->state = IMAGE_RELOAD_DONE;

	return ret;
}

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
	struct reload_hp_controller *hpc, *tmp;
	struct dfl_image_reload *reload;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		reload = &hpc->reload;
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

static void dfl_hp_add_reload_dev(struct dfl_image_reload_priv *priv, struct reload_hp_controller *hpc)
{
	mutex_lock(&priv->lock);
	list_add(&hpc->node, &priv->dev_list);
	mutex_unlock(&priv->lock);
}

static int dfl_hp_init_controller(struct controller *ctrl, struct pcie_device *dev)
{
	u32 slot_cap;
	struct pci_dev *hotplug_bridge = dev->port;
	struct pci_bus *subordinate = hotplug_bridge->subordinate;

	ctrl->pcie = dev;

	pcie_capability_read_dword(hotplug_bridge, PCI_EXP_SLTCAP, &slot_cap);

	/* set the Power Controller Present */
	slot_cap |= PCI_EXP_SLTCAP_PCP;

	ctrl->slot_cap = slot_cap;
	mutex_init(&ctrl->ctrl_lock);
	mutex_init(&ctrl->state_lock);
	init_rwsem(&ctrl->reset_lock);
	init_waitqueue_head(&ctrl->requester);
	init_waitqueue_head(&ctrl->queue);

	return 0;
}

static const struct hotplug_slot_ops dfl_hotplug_slot_ops = {
        .available_images       = dfl_hotplug_available_images,
        .image_reload           = dfl_hotplug_image_reload
};

static int dfl_hp_init_slot(struct controller *ctrl)
{
	char name[SLOT_NAME_SIZE];
	struct pci_dev *hotplug_bridge = ctrl->pcie->port;
	int ret;

	printk("%s: pcidev %lx\n", __func__, (unsigned long)hotplug_bridge);

	snprintf(name, SLOT_NAME_SIZE, "%u", (ctrl->slot_cap & PCI_EXP_SLTCAP_PSN) >> 19);
	
	ctrl->hotplug_slot.ops = &dfl_hotplug_slot_ops;

	 /* Register PCI slot */
        ret = pci_hp_register(&ctrl->hotplug_slot, hotplug_bridge->subordinate,
			PCI_SLOT(hotplug_bridge->devfn), name);
        if (ret) {
                pr_err("pci_hp_register failed with error %d\n", ret);
                return ret;
        }

        pr_info("Slot [%s] registered\n", hotplug_slot_name(&ctrl->hotplug_slot));

	return ret;
}

static int
dfl_hp_create_new_hpc(struct reload_hp_controller *hpc, struct pci_dev *hotplug_bridge, 
		const char * name, const struct dfl_image_reload_ops *ops, void *priv)
{
	struct dfl_image_reload *reload = &hpc->reload;
	struct pcie_device *pcie;
	struct controller *ctrl;
	int ret;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->port = hotplug_bridge;
	hpc->hotplug_bridge = hotplug_bridge;
	hpc->pcie = pcie;

	dfl_hp_init_controller(&hpc->ctrl, pcie);

	ret = dfl_hp_init_slot(&hpc->ctrl);
	if (ret) {
		if (ret == -EBUSY)
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n");
		else
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", ret);
		goto free_pcie;
	}

	mutex_init(&reload->lock);

	dfl_hp_add_reload_dev(dfl_priv, hpc);

	return ret;

free_pcie:
	kfree(pcie);
	return ret;
}

static struct reload_hp_controller *
dfl_hp_find_exist_hpc(struct pci_dev *hotplug_bridge, 
		struct pci_dev *pcidev, const struct dfl_image_reload_ops *ops)
{
	struct reload_hp_controller *hpc, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		if (!hpc->reload.is_registered)
			continue;
		if ((hpc->hotplug_bridge == hotplug_bridge)
				&& (hpc->reload.priv == pcidev)
				&& (hpc->reload.ops == ops)) {
			mutex_unlock(&dfl_priv->lock);
			printk("%s found existing \n", __func__);
			return hpc;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static struct reload_hp_controller *dfl_hp_reclaim_hpc(struct pci_dev *hotplug_bridge)
{
	struct reload_hp_controller *hpc, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		/* skip using hpc */
		if (hpc->reload.is_registered)
			continue;

		/* reclaim unused hpc, will reuse it later */
		if (hpc->hotplug_bridge == hotplug_bridge) {
			printk("%s reuse it %s \n", __func__, hpc->reload.name);
			mutex_unlock(&dfl_priv->lock);
			return hpc;
		}

		/* free unused hpc */
		if (hpc->reload.is_registered && hpc->reload.state == IMAGE_RELOAD_DONE) {
			list_del(&hpc->node);
			printk("%s free it %s \n", __func__, hpc->reload.name);
			pci_hp_deregister(&hpc->ctrl.hotplug_slot);
			kfree(hpc);
			continue;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static void dfl_image_reload_remove_devs(void)
{
	struct reload_hp_controller *hpc, *tmp;
	struct controller *ctrl;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		list_del(&hpc->node);
		ctrl = &hpc->ctrl;
		printk("%s ===== %s \n", __func__, hpc->reload.name);
		pci_hp_deregister(&ctrl->hotplug_slot);
		kfree(hpc);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct dfl_image_reload_ops *ops, void *priv)
{
	struct pci_dev *pcidev, *hotplug_bridge;
	struct reload_hp_controller *hpc;
	struct dfl_image_reload *reload;
	int ret;

	if (!ops || !priv)
		return ERR_PTR(-EINVAL);

	pcidev = (struct pci_dev *)priv;

	dev_dbg(&pcidev->dev, "registering pci: %04x:%02x:%02x.%d to reload driver\n",
			pci_domain_nr(pcidev->bus), pcidev->bus->number,
			PCI_SLOT(pcidev->devfn), PCI_FUNC(pcidev->devfn));

	/* For N3000, the hotplug bridge was on the root port of PF0 */
	hotplug_bridge = pcie_find_root_port(pcidev);
	if (!hotplug_bridge)
		return ERR_PTR(-EINVAL);

	printk("%s hotplug_dev %lx pcidev %lx\n",__func__, (unsigned long)hotplug_bridge, (unsigned long)pcidev);

	dev_dbg(&pcidev->dev, "hotplug bridge: %04x:%02x:%02x\n",
			pci_domain_nr(hotplug_bridge->bus), hotplug_bridge->bus->number,
			PCI_SLOT(hotplug_bridge->devfn));

	/* find exist matched hotplug controller */
	hpc = dfl_hp_find_exist_hpc(hotplug_bridge, pcidev, ops);
	if (hpc)
		return &hpc->reload;

	/* can it reuse the free hotplug controller? */
	hpc = dfl_hp_reclaim_hpc(hotplug_bridge);
	if (hpc) {
		printk("%s can reuse\n", __func__);
		goto reuse;
	}

	printk("%s create a new one\n", __func__);
	hpc = kzalloc(sizeof(*hpc), GFP_KERNEL);
	if (!hpc)
		return ERR_PTR(-ENOMEM);

	/* create new reload dev */
	ret = dfl_hp_create_new_hpc(hpc, hotplug_bridge, name, ops, priv);
	if (ret) {
		kfree(hpc);
		return ERR_PTR(ret);
	}

reuse:
	printk("%s re-init hc\n", __func__);
	mutex_lock(&hpc->reload.lock);
	hpc->reload.ops = ops;
	hpc->reload.name = name;
	hpc->reload.priv = priv;
	hpc->reload.is_registered = true;
	hpc->reload.state = IMAGE_RELOAD_UNKNOWN;
	mutex_unlock(&hpc->reload.lock);

	return &hpc->reload;
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

MODULE_DESCRIPTION("DFL FPGA Image Reload Hotplug Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
