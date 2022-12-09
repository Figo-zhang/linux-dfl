// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA PCI Hotplug Manager Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#include <linux/delay.h>
#include <linux/fpga/fpgahp_manager.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "pciehp.h"

struct fpgahp_priv {
	struct list_head dev_list;
	struct mutex lock;	/* protects dev_list */
};

struct fpgahp_controller {
	struct list_head node;
	struct pcie_device *pcie;
	struct controller ctrl;
	struct pci_dev *hotplug_bridge;
	struct fpgahp_manager mgr;
};

static struct fpgahp_priv *fpgahp_priv;

static inline struct fpgahp_controller *to_hpc(struct controller *ctrl)
{
	return container_of(ctrl, struct fpgahp_controller, ctrl);
}

static int fpgahp_available_images(struct hotplug_slot *slot, char *buf)
{
	return 0;
}

static int fpgahp_image_load(struct hotplug_slot *slot, const char *buf)
{
	return 0;
}

static void fpgahp_add_hpc(struct fpgahp_priv *priv,
			   struct fpgahp_controller *hpc)
{
	mutex_lock(&priv->lock);
	list_add(&hpc->node, &priv->dev_list);
	mutex_unlock(&priv->lock);
}

static int fpgahp_init_controller(struct controller *ctrl, struct pcie_device *dev)
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

static const struct hotplug_slot_ops fpgahp_slot_ops = {
	.available_images       = fpgahp_available_images,
	.image_load             = fpgahp_image_load
};

static int fpgahp_init_slot(struct controller *ctrl)
{
	char name[SLOT_NAME_SIZE];
	struct pci_dev *hotplug_bridge = ctrl->pcie->port;
	int ret;

	snprintf(name, sizeof(name), "%u", PSN(ctrl));

	ctrl->hotplug_slot.ops = &fpgahp_slot_ops;

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
fpgahp_create_new_hpc(struct fpgahp_controller *hpc, struct pci_dev *hotplug_bridge,
		      const char *name, const struct fpgahp_manager_ops *ops)
{
	struct fpgahp_manager *mgr = &hpc->mgr;
	struct controller *ctrl = &hpc->ctrl;
	struct pcie_device *pcie;
	int ret;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->port = hotplug_bridge;
	hpc->hotplug_bridge = hotplug_bridge;
	hpc->pcie = pcie;

	fpgahp_init_controller(ctrl, pcie);

	ret = fpgahp_init_slot(ctrl);
	if (ret) {
		if (ret == -EBUSY)
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n");
		else
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", ret);
		goto free_pcie;
	}

	mutex_init(&mgr->lock);

	fpgahp_add_hpc(fpgahp_priv, hpc);

	return ret;

free_pcie:
	kfree(pcie);
	return ret;
}

static struct fpgahp_controller *
fpgahp_find_exist_hpc(struct pci_dev *hotplug_bridge,
		      struct pci_dev *pcidev, const struct fpgahp_manager_ops *ops)
{
	struct fpgahp_controller *hpc, *tmp;

	mutex_lock(&fpgahp_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &fpgahp_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		if (!hpc->mgr.is_registered)
			continue;

		if (hpc->hotplug_bridge == hotplug_bridge &&
		    hpc->mgr.priv == pcidev &&
		    hpc->mgr.ops == ops) {
			mutex_unlock(&fpgahp_priv->lock);
			ctrl_dbg(ctrl, "reuse hpc slot(%s)\n", slot_name(ctrl));
			return hpc;
		}
	}

	mutex_unlock(&fpgahp_priv->lock);

	return NULL;
}

static struct fpgahp_controller *fpgahp_reclaim_hpc(struct pci_dev *hotplug_bridge)
{
	struct fpgahp_controller *hpc, *tmp;

	mutex_lock(&fpgahp_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &fpgahp_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		if (hpc->mgr.is_registered)
			continue;

		/* reclaim unused hpc, will reuse it later */
		if (hpc->hotplug_bridge == hotplug_bridge) {
			ctrl_dbg(ctrl, "reuse hpc slot(%s)\n", slot_name(ctrl));
			mutex_unlock(&fpgahp_priv->lock);
			return hpc;
		}

		/* free unused hpc */
		if (hpc->mgr.is_registered && hpc->mgr.state == FPGAHP_MGR_LOAD_DONE) {
			list_del(&hpc->node);
			ctrl_dbg(ctrl, "free hpc slot(%s)\n", slot_name(ctrl));
			pci_hp_deregister(&ctrl->hotplug_slot);
			kfree(hpc);
			continue;
		}
	}

	mutex_unlock(&fpgahp_priv->lock);

	return NULL;
}

static void fpgahp_remove_hpc(void)
{
	struct fpgahp_controller *hpc, *tmp;

	mutex_lock(&fpgahp_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &fpgahp_priv->dev_list, node) {
		struct controller *ctrl = &hpc->ctrl;

		list_del(&hpc->node);
		pci_hp_deregister(&ctrl->hotplug_slot);
		kfree(hpc);
	}

	mutex_unlock(&fpgahp_priv->lock);
}

/**
 * fpgahp_register - register fpga device into fpgahp driver
 * @hotplug_bridge: the hotplug bridge of the fpga device
 * @name: the name of the fpga device
 * @ops: pointer to structure of fpgahp manager ops
 * @priv: private data for fpga device
 *
 * Return: pointer to struct fpgahp_manager pointer or ERR_PTR()
 */
struct fpgahp_manager *fpgahp_register(struct pci_dev *hotplug_bridge, const char *name,
				       const struct fpgahp_manager_ops *ops, void *priv)
{
	struct pci_dev *pcidev;
	struct fpgahp_controller *hpc;
	int ret;

	if (!ops || !priv)
		return ERR_PTR(-EINVAL);

	pcidev = priv;

	dev_dbg(&pcidev->dev, "register hotplug bridge: %04x:%02x:%02x\n",
		pci_domain_nr(hotplug_bridge->bus), hotplug_bridge->bus->number,
		PCI_SLOT(hotplug_bridge->devfn));

	/* find exist matched hotplug controller */
	hpc = fpgahp_find_exist_hpc(hotplug_bridge, pcidev, ops);
	if (hpc)
		return &hpc->mgr;

	/* can it reuse the free hotplug controller? */
	hpc = fpgahp_reclaim_hpc(hotplug_bridge);
	if (hpc)
		goto reuse;

	hpc = kzalloc(sizeof(*hpc), GFP_KERNEL);
	if (!hpc)
		return ERR_PTR(-ENOMEM);

	/* create new reload dev */
	ret = fpgahp_create_new_hpc(hpc, hotplug_bridge, name, ops);
	if (ret) {
		kfree(hpc);
		return ERR_PTR(ret);
	}

reuse:
	mutex_lock(&hpc->mgr.lock);
	hpc->mgr.ops = ops;
	hpc->mgr.name = name;
	hpc->mgr.priv = pcidev;
	hpc->mgr.is_registered = true;
	hpc->mgr.state = FPGAHP_MGR_UNKNOWN;
	mutex_unlock(&hpc->mgr.lock);

	return &hpc->mgr;
}
EXPORT_SYMBOL_GPL(fpgahp_register);

/**
 * fpgahp_unregister - unregister fpga device from fpgahp driver
 * @mgr: point to the fpgahp_manager
 */
void fpgahp_unregister(struct fpgahp_manager *mgr)
{
	mutex_lock(&mgr->lock);
	mgr->is_registered = false;
	mutex_unlock(&mgr->lock);
}
EXPORT_SYMBOL_GPL(fpgahp_unregister);

static int __init fpgahp_init(void)
{
	struct fpgahp_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->dev_list);
	fpgahp_priv = priv;

	return 0;
}

static void __exit fpgahp_exit(void)
{
	fpgahp_remove_hpc();
	kfree(fpgahp_priv);
}

module_init(fpgahp_init);
module_exit(fpgahp_exit);

MODULE_DESCRIPTION("FPGA PCI Hotplug Manager Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
