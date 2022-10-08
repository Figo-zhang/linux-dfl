// SPDX-License-Identifier: GPL-2.0
/*
 * Intel DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2019-2022 Intel Corporation. All rights reserved.
 *
 */
#if 0
#include <linux/dfl.h>
#include <linux/pci.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include "dfl.h"
#include "dfl-image-reload.h"

#define DFL_IMAGE_RELOAD_XA_LIMIT  XA_LIMIT(0, INT_MAX)
static DEFINE_XARRAY_ALLOC(dfl_image_reload_xa);

static struct class *dfl_image_reload_class;
static struct dfl_image_reload *dfl_reload;

#define to_dfl_image_reload(d) container_of(d, struct dfl_image_reload, dev)

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

static ssize_t available_images_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct dfl_image_reload *dfl_reload = to_dfl_image_reload(dev);
	struct dfl_image_trigger *trigger = &dfl_reload->trigger;
	ssize_t count = 0;

	if (!dfl_reload->ops || !dfl_reload->priv ||
	    !trigger->ops || !trigger->priv)
		return -EINVAL;

	if (!trigger->ops->available_images)
		return -EINVAL;

	mutex_lock(&dfl_reload->lock);
	count = trigger->ops->available_images(trigger, buf);
	mutex_unlock(&dfl_reload->lock);

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

static ssize_t image_reload_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dfl_image_reload *dfl_reload = to_dfl_image_reload(dev);
	struct pci_dev *pcidev, *root;
	struct dfl_image_trigger *trigger = &dfl_reload->trigger;
	int ret = -EINVAL;

	if (!dfl_reload->ops || !dfl_reload->priv ||
	    !trigger->ops || !trigger->priv)
		return -EINVAL;

	pcidev = dfl_reload->priv;

	root = pcie_find_root_port(pcidev);
	if (!root)
		return -EINVAL;

	mutex_lock(&dfl_reload->lock);

	/* 1. remove all PFs and VFs except the PF0*/
	dfl_reload_remove_sibling_pci_dev(pcidev);

	/* 2. remove all non-reserved devices */
	if (dfl_reload->ops->prepare) {
		ret = dfl_reload->ops->prepare(dfl_reload);
		if (ret) {
			dev_err(&dfl_reload->dev, "prepare image reload failed\n");
			goto out;
		}
	}

	/* 3. trigger image reload */
	if (trigger->ops->image_trigger) {
		ret = trigger->ops->image_trigger(trigger, buf);
		if (ret) {
			dev_err(&dfl_reload->dev, "image trigger failed\n");
			goto out;
		}
	}

	/* 4. disable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, true);
	if (ret) {
		dev_err(&dfl_reload->dev, "disable root pcie link failed\n");
		goto out;
	}

	/* 5. remove reserved devices under FP0 and PCI devices under root hub*/
	pci_stop_and_remove_bus_device_locked(root);

	/* 6. Wait for FPGA/BMC reload done. eg, 10s */
	msleep(RELOAD_TIMEOUT_MS);

	/* 7. enable pci root hub link */
	ret = dfl_reload_disable_pcie_link(root, false);
	if (ret) {
		dev_err(&dfl_reload->dev, "enable root pcie link failed\n");
		goto out;
	}

	/* 8. rescan the PCI bus*/
	dfl_reload_rescan_pci_bus();

out:
	mutex_unlock(&dfl_reload->lock);

	return ret ? : count;
}

static DEVICE_ATTR_RO(available_images);
static DEVICE_ATTR_WO(image_reload);

static struct attribute *dfl_image_reload_attrs[] = {
	&dev_attr_available_images.attr,
	&dev_attr_image_reload.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dfl_image_reload);

struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops, void *priv)
{
	struct dfl_image_trigger *trigger = &dfl_reload->trigger;

	if (!ops) {
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	trigger->priv = priv;
	trigger->ops = ops;

	return trigger;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_register);

void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger)
{
	trigger->priv = NULL;
	trigger->ops = NULL;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_trigger_unregister);

struct dfl_image_reload *
dfl_image_reload_dev_register(const struct dfl_image_reload_ops *ops, void *priv)
{
	if (!ops) {
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	dfl_reload->priv = priv;
	dfl_reload->ops = ops;

	return dfl_reload;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_register);

void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload)
{
	dfl_reload->priv = NULL;
	dfl_reload->ops = NULL;
}
EXPORT_SYMBOL_GPL(dfl_image_reload_dev_unregister);

static void dfl_image_reload_dev_release(struct device *dev)
{
	struct dfl_image_reload *dfl_reload = to_dfl_image_reload(dev);

	xa_erase(&dfl_image_reload_xa, dfl_reload->dev.id);
}

static int __init dfl_image_reload_init(void)
{
	int ret;

	dfl_image_reload_class = class_create(THIS_MODULE, "dfl_image_reload");
	if (IS_ERR(dfl_image_reload_class))
		return PTR_ERR(dfl_image_reload_class);

	dfl_image_reload_class->dev_groups = dfl_image_reload_groups;
	dfl_image_reload_class->dev_release = dfl_image_reload_dev_release;

	dfl_reload = kzalloc(sizeof(*dfl_reload), GFP_KERNEL);
	if (!dfl_reload) {
		ret = -ENOMEM;
		goto free_class;
	}

	ret = xa_alloc(&dfl_image_reload_xa, &dfl_reload->dev.id,
		       dfl_reload, DFL_IMAGE_RELOAD_XA_LIMIT, GFP_KERNEL);
	if (ret)
		goto error_kfree;

	dfl_reload->dev.class = dfl_image_reload_class;
	dfl_reload->dev.parent = NULL;

	ret = dev_set_name(&dfl_reload->dev, "dfl_reload%d", dfl_reload->dev.id);
	if (ret) {
		dev_err(&dfl_reload->dev, "Failed to set device name: dfl_reload%d\n",
			dfl_reload->dev.id);
		goto error_device;
	}

	ret = device_register(&dfl_reload->dev);
	if (ret) {
		put_device(&dfl_reload->dev);
		goto error_device;
	}

	mutex_init(&dfl_reload->lock);

	return 0;

error_device:
	xa_erase(&dfl_image_reload_xa, dfl_reload->dev.id);
error_kfree:
	kfree(dfl_reload);
free_class:
	class_destroy(dfl_image_reload_class);

	return ret;
}

static void __exit dfl_image_reload_exit(void)
{
	device_unregister(&dfl_reload->dev);

	class_destroy(dfl_image_reload_class);

	kfree(dfl_reload);
}

module_init(dfl_image_reload_init);
module_exit(dfl_image_reload_exit);

MODULE_DESCRIPTION("DFL FPGA Image Reload Driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL");
#endif


#define pr_fmt(fmt) "FPGA RELOAD: " fmt
#define dev_fmt pr_fmt

// RUSS: Which of headers are actually needed?

#include <linux/bitops.h>
#include <linux/cper.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <acpi/apei.h>
#include <ras/ras_event.h>

#include "../pci/pci.h"
#include "../pci/pcie/portdrv.h"

struct fpga_reload_rpc {
/*	struct pci_dev *rpd;		/* Root Port device */
	struct pci_dev *fpga_dev;	/* FPGA device */
};

void pci_fpga_reload_init(struct pci_dev *dev)
{
}

void pci_fpga_reload_exit(struct pci_dev *dev)
{
}

static ssize_t
fpga_reload_show(struct device *dev, struct device_attribute *attr,
		 char *buf)
{
	struct pci_dev *root = to_pci_dev(dev);
	struct pcie_device *fpga_reload;
	struct fpga_reload_rpc *rpc;
	struct device *device;

	pci_info(root, "%s: root-pcidev is: %p\n", __func__, root);

	device = pcie_port_find_device(root, PCIE_PORT_SERVICE_FPGA_RELOAD);
	if (!device) {
		pci_err(root, "%s: unable to find reload_service\n", __func__);
		return -ENODEV;
	}

	fpga_reload = to_pcie_device(device);
	pci_info(root, "%s: FPGA Service pci_dev: %p\n", __func__, fpga_reload);

	rpc = (struct fpga_reload_rpc *)get_service_data(fpga_reload);
	pci_info(root, "%s: fpga_reload_rpc is: %p\n", __func__, rpc);
	pci_info(root, "%s: fpga_dev is: %p\n", __func__, rpc->fpga_dev);

	return sysfs_emit(buf, "fpga_reload_show has been called\n");
}
static DEVICE_ATTR_RO(fpga_reload);

static struct attribute *pcie_fpga_reload_attrs[] = {
	&dev_attr_fpga_reload.attr,
	NULL
};

const struct attribute_group pcie_fpga_reload_attr_group = {
	.attrs  = pcie_fpga_reload_attrs,
};

int pcie_fpga_reload_register(struct pci_dev *fpga_dev)
{
	struct pcie_device *fpga_reload;
	struct fpga_reload_rpc *rpc;
	struct device *device;
	struct pci_dev *root;


	if (!fpga_dev)
		return -EINVAL;

	root = pcie_find_root_port(fpga_dev);
	if (!root) {
		pci_err(root, "%s: unable to find root port\n", __func__);
		return -ENODEV;
	}

	pci_info(root, "%s: fpga-pcidev is: %p\n", __func__, fpga_dev);
	pci_info(root, "%s: root-pcidev is: %p\n", __func__, root);

	device = pcie_port_find_device(root, PCIE_PORT_SERVICE_FPGA_RELOAD);
	if (!device) {
		pci_err(root, "%s: unable to find reload_service\n", __func__);
		return -ENODEV;
	}

	fpga_reload = to_pcie_device(device);
	pci_info(root, "%s: fpga_reload is: %p\n", __func__, fpga_reload);

	rpc = (struct fpga_reload_rpc *)get_service_data(fpga_reload);
	rpc->fpga_dev = fpga_dev;

	return 0;
}
EXPORT_SYMBOL_GPL(pcie_fpga_reload_register);

/**
 * fpga_reload_remove - clean up resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus unloads
 */
static void fpga_reload_remove(struct pcie_device *dev)
{
}

/**
 * fpga_reload_probe - initialize resources
 * @dev: pointer to the pcie_dev data structure
 *
 * Invoked when PCI Express bus loads the FPGA Reload service driver.
 */
static int fpga_reload_probe(struct pcie_device *dev)
{
	struct device *device = &dev->device;
	struct pci_dev *port = dev->port;
	struct fpga_reload_rpc *rpc;

	rpc = devm_kzalloc(device, sizeof(*rpc), GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;

	set_service_data(dev, rpc);

	pci_info(port, "%s: fpga_reload_rpc is: %p\n", __func__, rpc);

	/* Limit to Root Ports */
	if (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT)
		return -ENODEV;

	pci_info(port, "enabled\n");
	return 0;
}

static struct pcie_port_service_driver fpga_reload_driver = {
	.name		= "fpga_reload",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_FPGA_RELOAD,

	.probe		= fpga_reload_probe,
	.remove		= fpga_reload_remove,
};

/**
 * pcie_fpga_reload_init - register FPGA reload root service driver
 *
 * Invoked when FPGA reload root service driver is loaded.
 */
int __init pcie_fpga_reload_init(void)
{
	return pcie_port_service_register(&fpga_reload_driver);
}
EXPORT_SYMBOL_GPL(pcie_fpga_reload_init);
