#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "../pci.h"
#include "portdrv.h"
#include <linux/fpga/dfl-image-reload.h>

#define DFL_IMAGE_RELOAD_XA_LIMIT  XA_LIMIT(0, INT_MAX)
static DEFINE_XARRAY_ALLOC(dfl_image_reload_xa);

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

static ssize_t fpga_rescan_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct pci_dev *root = to_pci_dev(dev);
	struct pcie_device *pcie;
	struct dfl_image_reload *dfl_reload;
	struct device *device;
	struct pci_dev *pcidev;
	int ret = -EINVAL;

	device = pcie_port_find_device(root, PCIE_PORT_SERVICE_FPGA_RELOAD);
	if (!device) {
		pci_err(root, "%s: unable to find reload_service\n", __func__);
		return -ENODEV;
	}

	pcie = to_pcie_device(device);
	dfl_reload = (struct dfl_image_reload *)get_service_data(pcie);

	if (!dfl_reload->ops || !dfl_reload->priv)
		return -EINVAL;

	pcidev = dfl_reload->priv;

	mutex_lock(&dfl_reload->lock);

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

static ssize_t fpga_prepare_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct pci_dev *root = to_pci_dev(dev);
	struct pcie_device *pcie;
	struct dfl_image_reload *dfl_reload;
	struct device *device;
	struct pci_dev *pcidev;
	int ret = -EINVAL;

	device = pcie_port_find_device(root, PCIE_PORT_SERVICE_FPGA_RELOAD);
	if (!device) {
		pci_err(root, "%s: unable to find reload_service\n", __func__);
		return -ENODEV;
	}

	pcie = to_pcie_device(device);
	dfl_reload = (struct dfl_image_reload *)get_service_data(pcie);

	if (!dfl_reload->ops || !dfl_reload->priv)
		return -EINVAL;

	pcidev = dfl_reload->priv;

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

static DEVICE_ATTR_WO(fpga_prepare);
static DEVICE_ATTR_WO(fpga_rescan);

static struct attribute *pcie_fpga_reload_attrs[] = {
	&dev_attr_fpga_prepare.attr,
	&dev_attr_fpga_rescan.attr,
	NULL
};

static umode_t pcie_fpga_reload_is_visible(struct kobject *kobj,
					  struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct dfl_image_reload *dfl_reload = to_dfl_image_reload(dev);

	if (dfl_reload->ops) {
		printk("%s: found reload service\n", __func__);
		return a->mode;
	}

	printk("%s: canot found reload service\n", __func__);

	return 0;
}

const struct attribute_group pcie_fpga_reload_attr_group = {
	.attrs  = pcie_fpga_reload_attrs,
	//.is_visible = pcie_fpga_reload_is_visible,
};

static const struct attribute_group *pcie_fpga_reload_attr_groups[] = {
	&pcie_fpga_reload_attr_group,
	NULL
};

struct dfl_image_reload *
pcie_fpga_reload_register(struct pci_dev *fpga_dev, const struct dfl_image_reload_ops *ops)
{
	struct pcie_device *pcie_dev;
	struct dfl_image_reload *reload;
	struct device *device;
	struct pci_dev *root;

	if (!fpga_dev) {
		printk("%s: invalid fpga_dev\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	root = pcie_find_root_port(fpga_dev);
	if (!root) {
		pci_err(root, "%s: unable to find root port\n", __func__);
		return ERR_PTR(-ENODEV);
	}

	pci_info(root, "%s: fpga-pcidev is: %p\n", __func__, fpga_dev);
	pci_info(root, "%s: root-pcidev is: %p\n", __func__, root);

	device = pcie_port_find_device(root, PCIE_PORT_SERVICE_FPGA_RELOAD);
	if (!device) {
		pci_err(root, "%s: unable to find reload_service\n", __func__);
		return ERR_PTR(-ENODEV);
	}

	pcie_dev = to_pcie_device(device);
	pci_info(root, "%s: fpga_reload is: %p\n", __func__, pcie_dev);

	reload = (struct dfl_image_reload *)get_service_data(pcie_dev);
	reload->priv = fpga_dev;
	reload->ops = ops;

	printk("%s: done\n", __func__);

	return reload;
}
EXPORT_SYMBOL_GPL(pcie_fpga_reload_register);

void pcie_fpga_reload_unregister(struct dfl_image_reload *reload)
{
	reload->priv = NULL;
	reload->ops = NULL;
}
EXPORT_SYMBOL_GPL(pcie_fpga_reload_unregister);

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
	struct dfl_image_reload *reload;
	int ret;

	/* Limit to Root Ports */
	if (pci_pcie_type(port) != PCI_EXP_TYPE_ROOT_PORT)
		return -ENODEV;

	reload = kzalloc(sizeof(*reload), GFP_KERNEL);
	if (!reload)
		return -ENOMEM;

	set_service_data(dev, reload);

	reload->dev.groups = pcie_fpga_reload_attr_groups; 
	reload->dev.parent = device;

	ret = xa_alloc(&dfl_image_reload_xa, &reload->dev.id,
                       reload, DFL_IMAGE_RELOAD_XA_LIMIT, GFP_KERNEL);
        if (ret)
                goto error_kfree;

        ret = dev_set_name(&reload->dev, "dfl_reload%d", reload->dev.id);
        if (ret) {
                dev_err(&reload->dev, "Failed to set device name: dfl_reload%d\n",
                        reload->dev.id);
                goto error_device;
        }

        ret = device_register(&reload->dev);
        if (ret) {
                put_device(&reload->dev);
                goto error_device;
        }

	pci_info(port, "enabled\n");

	return 0;

error_device:
        xa_erase(&dfl_image_reload_xa, reload->dev.id);
error_kfree:
        kfree(reload);

	return ret;
}
static struct pcie_port_service_driver fpga_reload_driver = {
	.name		= "fpga_image_reload",
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
int pcie_fpga_reload_init(void)
{
	return pcie_port_service_register(&fpga_reload_driver);
}
EXPORT_SYMBOL_GPL(pcie_fpga_reload_init);
