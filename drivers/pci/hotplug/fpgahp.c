// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA PCI Hotplug Manager Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#include <linux/delay.h>
#include <linux/dfl.h>
#include <linux/fpga-dfl.h>
#include <linux/fpga/fpgahp_manager.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>

#include "../pci.h"
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
	struct controller *ctrl = to_ctrl(slot);
	struct fpgahp_controller *hpc = to_hpc(ctrl);
	struct fpgahp_manager *mgr = &hpc->mgr;
	struct fpgahp_bmc_device *bmc = &mgr->bmc;
	ssize_t count;

	if (!mgr->is_registered || !bmc->is_registered)
		return -EINVAL;

	if (!bmc->ops->available_images)
		return -EINVAL;

	mutex_lock(&ctrl->state_lock);
	count = bmc->ops->available_images(bmc, buf);
	mutex_unlock(&ctrl->state_lock);

	return count;
}

static void fpgahp_remove_sibling_pci_dev(struct pci_dev *pcidev)
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

static int fpgahp_link_enable(struct controller *ctrl)
{
	int retval = 0;

	retval = pciehp_link_enable(ctrl);
	if (retval) {
		ctrl_err(ctrl, "Can not enable the link!\n");
		return retval;
	}

	/* Check link training status */
	retval = pciehp_check_link_status(ctrl);
	if (retval) {
		ctrl_err(ctrl, "check link status fail!\n");
		return retval;
	}

	/* Check for a power fault */
	if (pciehp_query_power_fault(ctrl)) {
		ctrl_err(ctrl, "Slot(%s): Power fault\n", slot_name(ctrl));
		return retval;
	}

	return 0;
}

static int fpgahp_rescan_slot(struct controller *ctrl)
{
	int retval = 0;
	struct pci_bus *parent = ctrl->pcie->port->subordinate;

	retval = pciehp_configure_device(ctrl);
	if (retval && retval != -EEXIST) {
		ctrl_err(ctrl, "Cannot add device at %04x:%02x:00\n",
			 pci_domain_nr(parent), parent->number);
		return retval;
	}

	return 0;
}

static int fpgahp_image_load(struct hotplug_slot *slot, const char *buf)
{
	struct controller *ctrl = to_ctrl(slot);
	struct fpgahp_controller *hpc = to_hpc(ctrl);
	struct fpgahp_manager *mgr = &hpc->mgr;
	struct fpgahp_bmc_device *bmc = &mgr->bmc;
	struct pci_dev *pcidev;
	u32 wait_time_msec;
	int ret = -EINVAL;

	if (!mgr->is_registered || !bmc->is_registered)
		return -EINVAL;

	if (!bmc->ops->image_trigger)
		return -EINVAL;

	pcidev = mgr->priv;
	if (!pcidev)
		return -EINVAL;

	mutex_lock(&ctrl->state_lock);
	pm_runtime_get_sync(&ctrl->pcie->port->dev);

	mgr->state = FPGAHP_MGR_LOADING;

	/* 1. remove all PFs and VFs except the PF0 */
	fpgahp_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (mgr->ops->hotplug_prepare) {
		ret = mgr->ops->hotplug_prepare(mgr);
		if (ret) {
			ctrl_err(ctrl, "prepare hotplug failed\n");
			fpgahp_rescan_slot(ctrl);
			goto out;
		}
	}

	/* 3. trigger loading a new image of BMC */
	ret = bmc->ops->image_trigger(bmc, buf, &wait_time_msec);
	if (ret) {
		ctrl_err(ctrl, "image trigger failed\n");
		fpgahp_rescan_slot(ctrl);
		goto out;
	}

	/* 4. disable link of hotplug bridge */
	pciehp_link_disable(ctrl);

	/* 5. remove PCI devices below hotplug bridge */
	pciehp_unconfigure_device(ctrl, true);

	/* 6. wait for FPGA/BMC load done */
	msleep(wait_time_msec);

	/* 7. re-enable link */
	ret = fpgahp_link_enable(ctrl);
	if (ret)
		goto out;

	/* 8. enumerate PCI devices below hotplug bridge */
	ret = fpgahp_rescan_slot(ctrl);

out:
	if (ret)
		mgr->state = FPGAHP_MGR_HP_FAIL;
	else
		mgr->state = FPGAHP_MGR_LOAD_DONE;

	pm_runtime_put(&ctrl->pcie->port->dev);
	mutex_unlock(&ctrl->state_lock);

	return ret;
}

static bool fpgahp_match_bmc_dev(struct fpgahp_manager *mgr, struct device *parent)
{
	struct pci_dev *pcidev = mgr->priv;
	struct device *dev = &pcidev->dev;

	/*
	 * bmc device (like security dev) is a subordinate device under
	 * pci device, so check if the parent device of bmc device recursively
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
	struct fpgahp_controller *hpc, *tmp;

	mutex_lock(&fpgahp_priv->lock);

	list_for_each_entry_safe(hpc, tmp, &fpgahp_priv->dev_list, node) {
		struct fpgahp_manager *mgr = &hpc->mgr;

		if (!mgr->is_registered)
			continue;

		if (fpgahp_match_bmc_dev(mgr, parent)) {
			mutex_unlock(&fpgahp_priv->lock);
			return &mgr->bmc;
		}
	}
	mutex_unlock(&fpgahp_priv->lock);

	return NULL;
}

struct fpgahp_bmc_device *
fpgahp_bmc_device_register(const struct fpgahp_bmc_ops *ops,
			   struct device *parent, void *priv)
{
	struct fpgahp_manager *mgr;
	struct fpgahp_bmc_device *bmc;

	if (!ops)
		return ERR_PTR(-EINVAL);

	bmc = fpgahp_find_bmc(parent);
	if (!bmc)
		return ERR_PTR(-EINVAL);

	mgr = to_fpgahp_mgr(bmc);

	mutex_lock(&mgr->lock);
	bmc->priv = priv;
	bmc->parent = parent;
	bmc->ops = ops;
	bmc->is_registered = true;
	mutex_unlock(&mgr->lock);

	return bmc;
}
EXPORT_SYMBOL_GPL(fpgahp_bmc_device_register);

void fpgahp_bmc_device_unregister(struct fpgahp_bmc_device *bmc)
{
	struct fpgahp_manager *mgr = to_fpgahp_mgr(bmc);

	mutex_lock(&mgr->lock);
	bmc->is_registered = false;
	mutex_unlock(&mgr->lock);
}
EXPORT_SYMBOL_GPL(fpgahp_bmc_device_unregister);

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
