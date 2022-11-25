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

#define to_hpc(d) container_of(d, struct dfl_hp_controller, ctrl)

static int dfl_hp_available_images(struct hotplug_slot *slot, char *buf)
{
	return 0;
}

static int dfl_hp_image_reload(struct hotplug_slot *slot, const char *buf)
{
	return 0;
}

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

static void dfl_hp_remove_hpc(void)
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
dfl_hp_register_image_reload(const char *name, const struct dfl_image_reload_ops *ops, void *priv)
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

	/*
	 * For N3000 Card, FPGA devices like PFs/VFs and some Ethernet Controllers
	 * connected with a PCI switch, so the hotplug bridge was on the root
	 * port of FPGA PF0 device.
	 */
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
