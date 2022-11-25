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
#include <linux/fpga/dfl-image-reload.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "pciehp.h"

extern int pci_hp_add_bridge(struct pci_dev *dev);

struct dfl_hp_image_reload_priv {
	struct list_head dev_list;
	struct mutex lock; /* protect data structure contents */
};

struct dfl_hp_controller {
	struct list_head node;
	struct pcie_device *pcie;
	struct controller ctrl;
	struct pci_dev *hotplug_bridge;
	struct dfl_image_reload reload;
};

static struct dfl_hp_image_reload_priv *dfl_priv;

#define to_dfl_trigger_reload(d) container_of(d, struct dfl_image_reload, trigger)
#define to_hpc(d) container_of(d, struct dfl_hp_controller, ctrl)

static ssize_t dfl_hp_available_images(struct hotplug_slot *slot, char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct dfl_hp_controller *hpc = to_hpc(ctrl);
	struct dfl_image_reload *reload = &hpc->reload;
	struct dfl_image_trigger *trigger = &reload->trigger;
	ssize_t count;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->available_images)
		return -EINVAL;

	mutex_lock(&ctrl->state_lock);
	count = trigger->ops->available_images(trigger, buf);
	mutex_unlock(&ctrl->state_lock);

	return count;
}

static void dfl_hp_remove_sibling_pci_dev(struct pci_dev *pcidev)
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

static void dfl_hp_set_slot_off(struct controller *ctrl)
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

static int dfl_hp_set_slot_on(struct controller *ctrl)
{
	int retval;

	/*
	 * Turn on slot
	 */
	if (POWER_CTRL(ctrl)) {
		/* Power on slot */
		retval = pciehp_power_on_slot(ctrl);
		if (retval)
			return retval;

		msleep(1000);
	}

	return 0;
}

static int dfl_hp_rescan_slot(struct controller *ctrl)
{
	int retval = 0;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	/* Check link training status */
	retval = pciehp_check_link_status(ctrl);
	if (retval)
		goto err_exit;

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
	dfl_hp_set_slot_off(ctrl);
	return retval;
}

static int dfl_hp_image_reload(struct hotplug_slot *slot, const char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct dfl_hp_controller *hpc = to_hpc(ctrl);
	struct dfl_image_reload *reload = &hpc->reload;
	struct dfl_image_trigger *trigger = &reload->trigger;
	struct pci_dev *pcidev;
	u32 wait_time_sec;
	int ret = -EINVAL;

	if (!reload->is_registered || !trigger->is_registered)
		return -EINVAL;

	if (!trigger->ops->image_trigger)
		return -EINVAL;

	pcidev = reload->priv;
	if (!pcidev)
		return -EINVAL;

	mutex_lock(&ctrl->state_lock);
	pm_runtime_get_sync(&ctrl->pcie->port->dev);

	reload->state = IMAGE_RELOAD_RELOADING;

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_hp_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (reload->ops->reload_prepare) {
		ret = reload->ops->reload_prepare(reload);
		if (ret) {
			ctrl_err(ctrl, "prepare image reload failed\n");
			goto trigger_fail;
		}
	}

	/* 3. trigger image reload of BMC */
	ret = trigger->ops->image_trigger(trigger, buf, &wait_time_sec);
	if (ret) {
		ctrl_err(ctrl, "image trigger failed\n");
		goto trigger_fail;
	}

	/* 4. disable link of hotplug bridge */
	pciehp_link_disable(ctrl);

	/* 5. remove PCI devices below hotplug bridge */
	pciehp_unconfigure_device(ctrl, true);

	/* 6. wait for FPGA/BMC reload done */
	ssleep(wait_time_sec);

	/* 7. turn off slot */
	dfl_hp_set_slot_off(ctrl);

	/* 8. turn on slot*/
	ret = dfl_hp_set_slot_on(ctrl);
	if (ret)
		goto slot_on_fail;

	/* 9. enumerate PCI devices below a hotplug bridge*/
	ret = dfl_hp_rescan_slot(ctrl);
	if (ret)
		goto rescan_fail;

	reload->state = IMAGE_RELOAD_DONE;

	pm_runtime_put(&ctrl->pcie->port->dev);
	mutex_unlock(&ctrl->state_lock);

	return ret;

rescan_fail:
	dfl_hp_set_slot_on(ctrl);
trigger_fail:
	dfl_hp_rescan_slot(ctrl);
slot_on_fail:
	reload->state = IMAGE_RELOAD_FAIL;
	pm_runtime_put(&ctrl->pcie->port->dev);
	mutex_unlock(&ctrl->state_lock);
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
	struct dfl_hp_controller *hpc, *tmp;
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

static void dfl_hp_add_reload_dev(struct dfl_hp_image_reload_priv *priv,
				  struct dfl_hp_controller *hpc)
{
	mutex_lock(&priv->lock);
	list_add(&hpc->node, &priv->dev_list);
	mutex_unlock(&priv->lock);
}

static int dfl_hp_init_controller(struct controller *ctrl, struct pcie_device *dev)
{
	u32 slot_cap;
	struct pci_dev *hotplug_bridge = dev->port;

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

static const struct hotplug_slot_ops dfl_hp_slot_ops = {
	.available_images       = dfl_hp_available_images,
	.image_reload           = dfl_hp_image_reload
};

static int dfl_hp_init_slot(struct controller *ctrl)
{
	char name[SLOT_NAME_SIZE];
	struct pci_dev *hotplug_bridge = ctrl->pcie->port;
	int ret;

	snprintf(name, SLOT_NAME_SIZE, "%u", (ctrl->slot_cap & PCI_EXP_SLTCAP_PSN) >> 19);

	ctrl->hotplug_slot.ops = &dfl_hp_slot_ops;

	 /* Register PCI slot */
	ret = pci_hp_register(&ctrl->hotplug_slot, hotplug_bridge->subordinate,
			      PCI_SLOT(hotplug_bridge->devfn), name);
	if (ret) {
		ctrl_err(ctrl, "pci_hp_register failed with error %d\n", ret);
		return ret;
	}

	ctrl_info(ctrl, "Slot [%s] registered\n", hotplug_slot_name(&ctrl->hotplug_slot));

	return ret;
}

static int
dfl_hp_create_new_hpc(struct dfl_hp_controller *hpc, struct pci_dev *hotplug_bridge,
		      const char *name, const struct dfl_image_reload_ops *ops, void *priv)
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

static struct dfl_hp_controller *
dfl_hp_find_exist_hpc(struct pci_dev *hotplug_bridge,
		      struct pci_dev *pcidev, const struct dfl_image_reload_ops *ops)
{
	struct dfl_hp_controller *hpc, *tmp;
	struct controller *ctrl;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		ctrl = &hpc->ctrl;
		if (!hpc->reload.is_registered)
			continue;
		if (hpc->hotplug_bridge == hotplug_bridge &&
		    hpc->reload.priv == pcidev &&
		    hpc->reload.ops == ops) {
			mutex_unlock(&dfl_priv->lock);
			ctrl_dbg(ctrl, "%s reuse hpc slot(%s)\n", __func__, slot_name(&hpc->ctrl));
			return hpc;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static struct dfl_hp_controller *dfl_hp_reclaim_hpc(struct pci_dev *hotplug_bridge)
{
	struct dfl_hp_controller *hpc, *tmp;
	struct controller *ctrl;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		ctrl = &hpc->ctrl;
		/* skip using hpc */
		if (hpc->reload.is_registered)
			continue;

		/* reclaim unused hpc, will reuse it later */
		if (hpc->hotplug_bridge == hotplug_bridge) {
			ctrl_dbg(ctrl, "%s reuse hpc slot(%s)\n", __func__, slot_name(&hpc->ctrl));
			mutex_unlock(&dfl_priv->lock);
			return hpc;
		}

		/* free unused hpc */
		if (hpc->reload.is_registered && hpc->reload.state == IMAGE_RELOAD_DONE) {
			list_del(&hpc->node);
			ctrl_dbg(ctrl, "%s free hpc slot(%s)\n", __func__, slot_name(&hpc->ctrl));
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
	struct dfl_hp_controller *hpc, *tmp;
	struct controller *ctrl;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		list_del(&hpc->node);
		ctrl = &hpc->ctrl;
		pci_hp_deregister(&ctrl->hotplug_slot);
		kfree(hpc);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name, const struct dfl_image_reload_ops *ops, void *priv)
{
	struct pci_dev *pcidev, *hotplug_bridge;
	struct dfl_hp_controller *hpc;
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

	dev_dbg(&pcidev->dev, "hotplug bridge: %04x:%02x:%02x\n",
		pci_domain_nr(hotplug_bridge->bus), hotplug_bridge->bus->number,
		PCI_SLOT(hotplug_bridge->devfn));

	/* find exist matched hotplug controller */
	hpc = dfl_hp_find_exist_hpc(hotplug_bridge, pcidev, ops);
	if (hpc)
		return &hpc->reload;

	/* can it reuse the free hotplug controller? */
	hpc = dfl_hp_reclaim_hpc(hotplug_bridge);
	if (hpc)
		goto reuse;

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
	dfl_image_reload_remove_devs();
	kfree(dfl_priv);
}

module_init(dfl_hp_image_reload_init);
module_exit(dfl_hp_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Hotplug Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
