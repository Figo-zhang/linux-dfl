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
#include <linux/fpga/dfl-hp-image-reload.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "pciehp.h"

struct dfl_hp_image_reload_priv {
	struct list_head dev_list;
	struct mutex lock;	/* protects dev_list */
};

struct dfl_hp_controller {
	struct list_head node;
	struct pcie_device *pcie;
	struct controller ctrl;
	struct pci_dev *hotplug_bridge;
	struct dfl_image_reload reload;
};

static struct dfl_hp_image_reload_priv *dfl_priv;

static inline struct dfl_hp_controller *to_hpc(struct controller *ctrl)
{
	return container_of(ctrl, struct dfl_hp_controller, ctrl);
}

static int dfl_hp_available_images(struct hotplug_slot *slot, char *buf)
{
	return 0;
}

static int dfl_hp_image_reload(struct hotplug_slot *slot, const char *buf)
{
	return 0;
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

static struct dfl_image_trigger *dfl_find_trigger(struct device *parent)
{
	struct dfl_hp_controller *hpc, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		struct dfl_image_reload *reload = &hpc->reload;

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
dfl_hp_register_trigger(const struct dfl_image_trigger_ops *ops,
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
	trigger->is_registered = true;
	mutex_unlock(&reload->lock);

	return trigger;
}
EXPORT_SYMBOL_GPL(dfl_hp_register_trigger);

void dfl_hp_unregister_trigger(struct dfl_image_trigger *trigger)
{
	struct dfl_image_reload *reload = to_dfl_trigger_reload(trigger);

	mutex_lock(&reload->lock);
	trigger->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_hp_unregister_trigger);

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

	ctrl->slot_cap = slot_cap;
	mutex_init(&ctrl->ctrl_lock);
	mutex_init(&ctrl->state_lock);
	init_rwsem(&ctrl->reset_lock);
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

	snprintf(name, sizeof(name), "%u", PSN(ctrl));

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
		      const char *name, const struct dfl_image_reload_ops *ops)
{
	struct dfl_image_reload *reload = &hpc->reload;
	struct controller *ctrl = &hpc->ctrl;
	struct pcie_device *pcie;
	int ret;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->port = hotplug_bridge;
	hpc->hotplug_bridge = hotplug_bridge;
	hpc->pcie = pcie;

	dfl_hp_init_controller(ctrl, pcie);

	ret = dfl_hp_init_slot(ctrl);
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

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		if (!hpc->reload.is_registered)
			continue;

		if (hpc->hotplug_bridge == hotplug_bridge &&
		    hpc->reload.priv == pcidev &&
		    hpc->reload.ops == ops) {
			mutex_unlock(&dfl_priv->lock);
			ctrl_dbg(ctrl, "reuse hpc slot(%s)\n", slot_name(ctrl));
			return hpc;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static struct dfl_hp_controller *dfl_hp_reclaim_hpc(struct pci_dev *hotplug_bridge)
{
	struct dfl_hp_controller *hpc, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		if (hpc->reload.is_registered)
			continue;

		/* reclaim unused hpc, will reuse it later */
		if (hpc->hotplug_bridge == hotplug_bridge) {
			ctrl_dbg(ctrl, "reuse hpc slot(%s)\n", slot_name(ctrl));
			mutex_unlock(&dfl_priv->lock);
			return hpc;
		}

		/* free unused hpc */
		if (hpc->reload.is_registered && hpc->reload.state == IMAGE_RELOAD_DONE) {
			list_del(&hpc->node);
			ctrl_dbg(ctrl, "free hpc slot(%s)\n", slot_name(ctrl));
			pci_hp_deregister(&ctrl->hotplug_slot);
			kfree(hpc);
			continue;
		}
	}

	mutex_unlock(&dfl_priv->lock);

	return NULL;
}

static void dfl_hp_remove_hpc(void)
{
	struct dfl_hp_controller *hpc, *tmp;

	mutex_lock(&dfl_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &dfl_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		list_del(&hpc->node);
		pci_hp_deregister(&ctrl->hotplug_slot);
		kfree(hpc);
	}

	mutex_unlock(&dfl_priv->lock);
}

struct dfl_image_reload *
dfl_hp_register_image_reload(struct pci_dev *pcidev, const char *name,
			     const struct dfl_image_reload_ops *ops)
{
	struct pci_dev *hotplug_bridge;
	struct dfl_hp_controller *hpc;
	int ret;

	if (!ops || !pcidev)
		return ERR_PTR(-EINVAL);

	dev_dbg(&pcidev->dev, "registering pci: %04x:%02x:%02x.%d to reload driver\n",
		pci_domain_nr(pcidev->bus), pcidev->bus->number,
		PCI_SLOT(pcidev->devfn), PCI_FUNC(pcidev->devfn));

	/*
	 * For N3000 Card, FPGA devices like PFs/VFs and some Ethernet Controllers
	 * connected with a PCI switch, so the hotplug bridge was on the root
	 * port of FPGA PF0 device.
	 */
	hotplug_bridge = pcie_find_root_port(pcidev);
	if (!hotplug_bridge)
		return ERR_PTR(-EINVAL);

	dev_dbg(&pcidev->dev, "found hotplug bridge: %04x:%02x:%02x\n",
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
	ret = dfl_hp_create_new_hpc(hpc, hotplug_bridge, name, ops);
	if (ret) {
		kfree(hpc);
		return ERR_PTR(ret);
	}

reuse:
	mutex_lock(&hpc->reload.lock);
	hpc->reload.ops = ops;
	hpc->reload.name = name;
	hpc->reload.priv = pcidev;
	hpc->reload.is_registered = true;
	hpc->reload.state = IMAGE_RELOAD_UNKNOWN;
	mutex_unlock(&hpc->reload.lock);

	return &hpc->reload;
}
EXPORT_SYMBOL_GPL(dfl_hp_register_image_reload);

void dfl_hp_unregister_image_reload(struct dfl_image_reload *reload)
{
	mutex_lock(&reload->lock);
	reload->is_registered = false;
	mutex_unlock(&reload->lock);
}
EXPORT_SYMBOL_GPL(dfl_hp_unregister_image_reload);

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
	dfl_hp_remove_hpc();
	kfree(dfl_priv);
}

module_init(dfl_hp_image_reload_init);
module_exit(dfl_hp_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Hotplug Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
