// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA PCI Hotplug Manager Driver
 *
 * Copyright (C) 2023 Intel Corporation
 */

#include <linux/fpga/fpgahp_manager.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>

#include "pciehp.h"

/*
 * a global fhpc_list is used to manage all
 * registered FPGA hotplug controllers.
 */
static LIST_HEAD(fhpc_list);
static DEFINE_MUTEX(fhpc_lock);

struct fpgahp_controller {
	struct list_head node;
	struct fpgahp_manager mgr;
	struct pcie_device *pcie;
	struct controller ctrl;
	struct pci_dev *hotplug_bridge;
};

static void fpgahp_add_fhpc(struct fpgahp_controller *fhpc)
{
	mutex_lock(&fhpc_lock);
	list_add_tail(&fhpc->node, &fhpc_list);
	mutex_unlock(&fhpc_lock);
}

static int fpgahp_init_controller(struct controller *ctrl, struct pcie_device *dev)
{
	struct pci_dev *hotplug_bridge = dev->port;
	u32 slot_cap;

	ctrl->pcie = dev;

	if (pcie_capability_read_dword(hotplug_bridge, PCI_EXP_SLTCAP, &slot_cap))
		return -EINVAL;

	ctrl->slot_cap = slot_cap;

	return 0;
}

static const struct hotplug_slot_ops fpgahp_slot_ops = {
};

static int fpgahp_init_slot(struct controller *ctrl)
{
	char name[SLOT_NAME_SIZE];
	struct pci_dev *hotplug_bridge = ctrl->pcie->port;
	int ret;

	snprintf(name, sizeof(name), "%u", PSN(ctrl));

	ctrl->hotplug_slot.ops = &fpgahp_slot_ops;

	ret = pci_hp_register(&ctrl->hotplug_slot, hotplug_bridge->subordinate,
			      PCI_SLOT(hotplug_bridge->devfn), name);
	if (ret) {
		ctrl_err(ctrl, "Register PCI hotplug core failed with error %d\n", ret);
		return ret;
	}

	ctrl_info(ctrl, "Slot [%s] registered\n", hotplug_slot_name(&ctrl->hotplug_slot));

	return 0;
}

static int
fpgahp_create_new_fhpc(struct fpgahp_controller *fhpc, struct pci_dev *hotplug_bridge,
		       const char *name, const struct fpgahp_manager_ops *ops)
{
	struct fpgahp_manager *mgr = &fhpc->mgr;
	struct controller *ctrl = &fhpc->ctrl;
	struct pcie_device *pcie;
	int ret;

	pcie = kzalloc(sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->port = hotplug_bridge;
	fhpc->hotplug_bridge = hotplug_bridge;
	fhpc->pcie = pcie;

	ret = fpgahp_init_controller(ctrl, pcie);
	if (ret)
		goto free_pcie;

	ret = fpgahp_init_slot(ctrl);
	if (ret) {
		if (ret == -EBUSY)
			ctrl_warn(ctrl, "Slot already registered by another hotplug driver\n");
		else
			ctrl_err(ctrl, "Slot initialization failed (%d)\n", ret);
		goto free_pcie;
	}

	mutex_init(&mgr->lock);

	fpgahp_add_fhpc(fhpc);

	return 0;

free_pcie:
	kfree(pcie);
	return ret;
}

static struct fpgahp_controller *
fpgahp_find_exist_fhpc(struct pci_dev *hotplug_bridge,
		       struct pci_dev *pcidev, const struct fpgahp_manager_ops *ops)
{
	struct fpgahp_controller *iter, *fhpc = NULL;

	mutex_lock(&fhpc_lock);

	list_for_each_entry(iter, &fhpc_list, node) {
		struct controller *ctrl = &iter->ctrl;

		if (!iter->mgr.registered)
			continue;

		if (iter->hotplug_bridge == hotplug_bridge &&
		    iter->mgr.priv == pcidev && iter->mgr.ops == ops) {
			fhpc = iter;
			ctrl_dbg(ctrl, "Found existing fhpc slot(%s)\n", slot_name(ctrl));
			break;
		}
	}

	mutex_unlock(&fhpc_lock);

	return fhpc;
}

static struct fpgahp_controller *fpgahp_reclaim_fhpc(struct pci_dev *hotplug_bridge)
{
	struct fpgahp_controller *iter, *fhpc = NULL;

	mutex_lock(&fhpc_lock);

	list_for_each_entry(iter, &fhpc_list, node) {
		struct controller *ctrl = &iter->ctrl;

		if (iter->mgr.registered)
			continue;

		/* reclaim unused fhpc, will reuse it later */
		if (iter->hotplug_bridge == hotplug_bridge) {
			fhpc = iter;
			ctrl_dbg(ctrl, "Found unused fhpc, reuse slot(%s)\n", slot_name(ctrl));
			break;
		}
	}

	mutex_unlock(&fhpc_lock);

	return fhpc;
}

static void fpgahp_remove_fhpc(void)
{
	struct fpgahp_controller *fhpc, *tmp;

	mutex_lock(&fhpc_lock);

	list_for_each_entry_safe(fhpc, tmp, &fhpc_list, node) {
		struct controller *ctrl = &fhpc->ctrl;

		list_del(&fhpc->node);
		pci_hp_deregister(&ctrl->hotplug_slot);
		kfree(fhpc);
	}

	mutex_unlock(&fhpc_lock);
}

/**
 * fpgahp_register - register FPGA device into fpgahp driver
 * @hotplug_bridge: the hotplug bridge of the FPGA device
 * @name: the name of the FPGA device
 * @ops: pointer to structure of fpgahp manager ops
 * @priv: private data for FPGA device
 *
 * Return: pointer to struct fpgahp_manager pointer or ERR_PTR()
 */
struct fpgahp_manager *fpgahp_register(struct pci_dev *hotplug_bridge, const char *name,
				       const struct fpgahp_manager_ops *ops, void *priv)
{
	struct fpgahp_controller *fhpc;
	struct pci_dev *pcidev = priv;
	int ret;

	if (!hotplug_bridge || !ops || !pcidev)
		return ERR_PTR(-EINVAL);

	dev_dbg(&pcidev->dev, "Register hotplug bridge: %04x:%02x:%02x\n",
		pci_domain_nr(hotplug_bridge->bus), hotplug_bridge->bus->number,
		PCI_SLOT(hotplug_bridge->devfn));

	/* find existing matching fpgahp_controller */
	fhpc = fpgahp_find_exist_fhpc(hotplug_bridge, pcidev, ops);
	if (fhpc)
		return &fhpc->mgr;

	/* can it reuse the free fpgahp_controller? */
	fhpc = fpgahp_reclaim_fhpc(hotplug_bridge);
	if (fhpc)
		goto reuse;

	fhpc = kzalloc(sizeof(*fhpc), GFP_KERNEL);
	if (!fhpc)
		return ERR_PTR(-ENOMEM);

	ret = fpgahp_create_new_fhpc(fhpc, hotplug_bridge, name, ops);
	if (ret) {
		kfree(fhpc);
		return ERR_PTR(ret);
	}

reuse:
	mutex_lock(&fhpc->mgr.lock);
	fhpc->mgr.ops = ops;
	fhpc->mgr.name = name;
	fhpc->mgr.priv = pcidev;
	fhpc->mgr.registered = true;
	fhpc->mgr.state = FPGAHP_MGR_UNKNOWN;
	mutex_unlock(&fhpc->mgr.lock);

	return &fhpc->mgr;
}
EXPORT_SYMBOL_NS_GPL(fpgahp_register, FPGAHP);

/**
 * fpgahp_unregister - unregister FPGA device from fpgahp driver
 * @mgr: point to the fpgahp_manager
 */
void fpgahp_unregister(struct fpgahp_manager *mgr)
{
	mutex_lock(&mgr->lock);
	mgr->registered = false;
	mutex_unlock(&mgr->lock);
}
EXPORT_SYMBOL_NS_GPL(fpgahp_unregister, FPGAHP);

static int __init fpgahp_init(void)
{
	return 0;
}
module_init(fpgahp_init);

static void __exit fpgahp_exit(void)
{
	fpgahp_remove_fhpc();
}
module_exit(fpgahp_exit);

MODULE_DESCRIPTION("FPGA PCI Hotplug Manager Driver");
MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_LICENSE("GPL");
