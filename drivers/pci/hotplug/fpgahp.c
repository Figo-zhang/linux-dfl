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
#include <linux/pm_runtime.h>

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
	struct mutex lock;  /* parallel access into image_load callback */
};

static inline struct fpgahp_controller *to_fhpc(struct controller *ctrl)
{
	return container_of(ctrl, struct fpgahp_controller, ctrl);
}

static int fpgahp_available_images(struct hotplug_slot *slot, char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct fpgahp_controller *fhpc = to_fhpc(ctrl);
	struct fpgahp_manager *mgr = &fhpc->mgr;
	struct fpgahp_bmc_device *bmc = &mgr->bmc;
	ssize_t count;

	mutex_lock(&mgr->lock);

	if (!mgr->registered || !bmc->registered)
		goto out;

	if (!bmc->ops->available_images)
		goto out;

	count = bmc->ops->available_images(bmc, buf);

	mutex_unlock(&mgr->lock);

	return count;

out:
	mutex_unlock(&mgr->lock);
	return -EINVAL;
}

static void fpgahp_remove_sibling_pci_dev(struct pci_dev *pcidev)
{
	struct pci_bus *bus = pcidev->bus;
	struct pci_dev *sibling, *tmp;

	if (!bus)
		return;

	list_for_each_entry_safe_reverse(sibling, tmp, &bus->devices, bus_list)
		if (sibling != pcidev)
			pci_stop_and_remove_bus_device_locked(sibling);
}

static int fpgahp_link_enable(struct controller *ctrl)
{
	int retval;

	retval = pciehp_link_enable(ctrl);
	if (retval) {
		ctrl_err(ctrl, "Can not enable the link!\n");
		return retval;
	}

	retval = pciehp_check_link_status(ctrl);
	if (retval) {
		ctrl_err(ctrl, "Check link status fail!\n");
		return retval;
	}

	retval = pciehp_query_power_fault(ctrl);
	if (retval)
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));

	return retval;
}

static int fpgahp_rescan_slot(struct controller *ctrl)
{
	int retval;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	retval = pciehp_configure_device(ctrl);
	if (retval && retval != -EEXIST)
		ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
			 pci_domain_nr(parent), parent->number);

	return retval;
}

static int __fpgahp_image_load(struct fpgahp_controller *fhpc, const char *buf)
{
	struct pci_dev *hotplug_bridge = fhpc->hotplug_bridge;
	struct fpgahp_manager *mgr = &fhpc->mgr;
	struct fpgahp_bmc_device *bmc = &mgr->bmc;
	struct controller *ctrl = &fhpc->ctrl;
	struct pci_dev *pcidev = mgr->priv;
	u32 wait_time_msec;
	int ret;

	ret = pm_runtime_resume_and_get(&hotplug_bridge->dev);
	if (ret)
		goto err;

	mutex_lock(&mgr->lock);

	if (!pcidev || !mgr->registered || !bmc->registered || !bmc->ops->image_trigger) {
		ret = -EINVAL;
		goto out;
	}

	mgr->state = FPGAHP_MGR_LOADING;

	/* 1. remove all PFs and VFs except the PF0 */
	fpgahp_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (mgr->ops->hotplug_prepare) {
		ret = mgr->ops->hotplug_prepare(mgr);
		if (ret) {
			ctrl_err(ctrl, "Prepare hotplug failed\n");
			goto out;
		}
	}

	/* 3. trigger loading a new image of BMC */
	ret = bmc->ops->image_trigger(bmc, buf, &wait_time_msec);
	if (ret) {
		ctrl_err(ctrl, "Image trigger failed\n");
		goto out;
	}

	/* 4. disable link of hotplug bridge */
	pciehp_link_disable(ctrl);

	/*
	 * unlock the mrg->lock temporarily to avoid the dead lock while re-gain
	 * the same lock on fpgahp_unregister() during remove PCI devices below the
	 * hotplug bridge
	 */
	mutex_unlock(&mgr->lock);

	/* 5. remove PCI devices below hotplug bridge */
	pciehp_unconfigure_device(ctrl, true);

	/* 6. wait for FPGA/BMC load done */
	msleep(wait_time_msec);

	mutex_lock(&mgr->lock);

	/* 7. re-enable link */
	ret = fpgahp_link_enable(ctrl);

out:
	if (ret)
		mgr->state = FPGAHP_MGR_HP_FAIL;
	else
		mgr->state = FPGAHP_MGR_LOAD_DONE;

	mutex_unlock(&mgr->lock);

	/* re-enumerate PCI devices below hotplug bridge */
	if (!ret)
		ret = fpgahp_rescan_slot(ctrl);

	pm_runtime_put(&hotplug_bridge->dev);
err:
	return ret;
}

static int fpgahp_image_load(struct hotplug_slot *slot, const char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct fpgahp_controller *fhpc = to_fhpc(ctrl);
	int ret;

	mutex_lock(&fhpc->lock);
	ret = __fpgahp_image_load(fhpc, buf);
	mutex_unlock(&fhpc->lock);

	return ret;
}

static void fpgahp_add_fhpc(struct fpgahp_controller *fhpc)
{
	mutex_lock(&fhpc_lock);
	list_add_tail(&fhpc->node, &fhpc_list);
	mutex_unlock(&fhpc_lock);
}

static struct fpgahp_bmc_device *fpgahp_find_bmc(struct device *bmc_device)
{
	struct fpgahp_bmc_device *bmc = NULL;
	struct fpgahp_controller *fhpc;

	mutex_lock(&fhpc_lock);

	list_for_each_entry(fhpc, &fhpc_list, node) {
		struct fpgahp_manager *mgr = &fhpc->mgr;
		struct pci_dev *pcidev = mgr->priv;

		if (!mgr->registered)
			continue;

		/*
		 * BMC device (like security dev) is a subordinate device under
		 * PCI device, so check if the parent device of BMC device recursively
		 */
		if (device_is_ancestor(&pcidev->dev, bmc_device)) {
			bmc = &mgr->bmc;
			break;
		}
	}

	mutex_unlock(&fhpc_lock);

	return bmc;
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
	bmc->device = dev;
	bmc->ops = ops;
	bmc->registered = true;
	mutex_unlock(&mgr->lock);

	return bmc;
}
EXPORT_SYMBOL_NS_GPL(fpgahp_bmc_device_register, FPGAHP);

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
EXPORT_SYMBOL_NS_GPL(fpgahp_bmc_device_unregister, FPGAHP);

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
	.available_images	= fpgahp_available_images,
	.image_load		= fpgahp_image_load,
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
	mutex_init(&fhpc->lock);

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
MODULE_IMPORT_NS(PCIEHP);
