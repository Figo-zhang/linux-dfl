// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA PCI Hotplug Manager Driver
 *
 * Copyright (C) 2022 Intel Corporation
 */
#include <linux/fpga/fpgahp_manager.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "pciehp.h"

/*
 * a global fhpc_list is used to manage all
 * of registered FPGA hotplug controllers.
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

static bool fpgahp_mgr_check_registered(struct fpgahp_manager *mgr)
{
	bool registered;

	mutex_lock(&mgr->lock);
	registered = mgr->registered;
	mutex_unlock(&mgr->lock);

	return registered;
}

static void fpgahp_add_fhpc(struct fpgahp_controller *fhpc)
{
	mutex_lock(&fhpc_lock);
	list_add_tail(&fhpc->node, &fhpc_list);
	mutex_unlock(&fhpc_lock);
}

static bool fpgahp_match_bmc_dev(struct fpgahp_manager *mgr, struct device *parent)
{
	struct pci_dev *pcidev = mgr->priv;
	struct device *dev = &pcidev->dev;

	/*
	 * BMC device (like security dev) is a subordinate device under
	 * PCI device, so check if the parent device of BMC device recursively
	 */
	while (parent) {
		if (parent == dev)
			return true;
		parent = parent->parent;
	}

	return false;
}

static struct fpgahp_bmc_device *fpgahp_find_bmc(struct device *parent)
{
	struct fpgahp_controller *fhpc;

	mutex_lock(&fhpc_lock);

	list_for_each_entry(fhpc, &fhpc_list, node) {
		struct fpgahp_manager *mgr = &fhpc->mgr;

		if (!fpgahp_mgr_check_registered(mgr))
			continue;

		if (fpgahp_match_bmc_dev(mgr, parent)) {
			mutex_unlock(&fhpc_lock);
			return &mgr->bmc;
		}
	}
	mutex_unlock(&fhpc_lock);

	return NULL;
}

/**
 * fpgahp_bmc_device_register - register FPGA BMC device into fpgahp driver
 * @ops: pointer to structure of fpgahp manager ops
 * @dev: device struct of BMC device
 * @priv: private data for FPGA device
 *
 * Return: pointer to struct fpgahp_manager pointer or ERR_PTR()
 */
struct fpgahp_bmc_device *
fpgahp_bmc_device_register(const struct fpgahp_bmc_ops *ops,
			   struct device *dev, void *priv)
{
	struct fpgahp_manager *mgr;
	struct fpgahp_bmc_device *bmc;

	if (!ops)
		return ERR_PTR(-EINVAL);

	bmc = fpgahp_find_bmc(dev);
	if (!bmc)
		return ERR_PTR(-EINVAL);

	mgr = to_fpgahp_mgr(bmc);

	mutex_lock(&mgr->lock);
	bmc->priv = priv;
	bmc->parent = dev;
	bmc->ops = ops;
	bmc->registered = true;
	mutex_unlock(&mgr->lock);

	return bmc;
}
EXPORT_SYMBOL_GPL(fpgahp_bmc_device_register);

/**
 * fpgahp_bmc_device_unregister - unregister FPGA BMC device from fpgahp driver
 * @bmc: point to the fpgahp_bmc_device
 */
void fpgahp_bmc_device_unregister(struct fpgahp_bmc_device *bmc)
{
	struct fpgahp_manager *mgr = to_fpgahp_mgr(bmc);

	mutex_lock(&mgr->lock);
	bmc->registered = false;
	mutex_unlock(&mgr->lock);
}
EXPORT_SYMBOL_GPL(fpgahp_bmc_device_unregister);

static int fpgahp_init_controller(struct controller *ctrl, struct pcie_device *dev)
{
	struct pci_dev *hotplug_bridge = dev->port;
	u32 slot_cap;

	ctrl->pcie = dev;

	if (pcie_capability_read_dword(hotplug_bridge, PCI_EXP_SLTCAP, &slot_cap))
		return -EINVAL;

	ctrl->slot_cap = slot_cap;
	mutex_init(&ctrl->ctrl_lock);
	mutex_init(&ctrl->state_lock);
	init_rwsem(&ctrl->reset_lock);
	init_waitqueue_head(&ctrl->queue);

	return 0;
}

static const struct hotplug_slot_ops fpgahp_slot_ops = {};

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
		ctrl_err(ctrl, "Pci_hp_register failed with error %d\n", ret);
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
	struct fpgahp_controller *fhpc;

	mutex_lock(&fhpc_lock);

	list_for_each_entry(fhpc, &fhpc_list, node) {
		struct controller *ctrl = &fhpc->ctrl;

		if (!fpgahp_mgr_check_registered(&fhpc->mgr))
			continue;

		if (fhpc->hotplug_bridge == hotplug_bridge &&
		    fhpc->mgr.priv == pcidev && fhpc->mgr.ops == ops) {
			mutex_unlock(&fhpc_lock);
			ctrl_dbg(ctrl, "Reuse fhpc slot(%s)\n", slot_name(ctrl));
			return fhpc;
		}
	}

	mutex_unlock(&fhpc_lock);

	return NULL;
}

static struct fpgahp_controller *fpgahp_reclaim_fhpc(struct pci_dev *hotplug_bridge)
{
	struct fpgahp_controller *fhpc, *tmp;

	mutex_lock(&fhpc_lock);

	list_for_each_entry_safe(fhpc, tmp, &fhpc_list, node) {
		struct controller *ctrl = &fhpc->ctrl;

		if (fpgahp_mgr_check_registered(&fhpc->mgr))
			continue;

		/* reclaim unused fhpc, will reuse it later */
		if (fhpc->hotplug_bridge == hotplug_bridge) {
			ctrl_dbg(ctrl, "Reuse fhpc slot(%s)\n", slot_name(ctrl));
			mutex_unlock(&fhpc_lock);
			return fhpc;
		}
	}

	mutex_unlock(&fhpc_lock);

	return NULL;
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
 * @priv:  private data for FPGA device
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

	/* find exist matched fpgahp_controller */
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

	/* create new fpgahp_controller */
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
EXPORT_SYMBOL_GPL(fpgahp_register);

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
EXPORT_SYMBOL_GPL(fpgahp_unregister);

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
