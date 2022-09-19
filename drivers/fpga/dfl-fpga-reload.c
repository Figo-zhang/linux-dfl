#include <linux/dfl.h>
#include <linux/pci.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include "dfl.h"
#include "dfl-fpga-reload.h"

#define DFL_FPGA_RELOAD_XA_LIMIT  XA_LIMIT(0, INT_MAX)
static DEFINE_XARRAY_ALLOC(dfl_fpga_reload_xa);

static struct class *dfl_fpga_reload_class;
static struct dfl_fpga_reload *dfl_reload;

#define to_dfl_fpga_reload(d) container_of(d, struct dfl_fpga_reload, dev)

static int dfl_fpga_disable_pcie_link(struct pci_dev *root, bool disable)
{
	u16 linkctl;
	int ret;

	if (!root)
		return -EINVAL;

	printk("%s=== %d\n", __func__, disable);

	printk("%04x:%02x:%02x.%d", pci_domain_nr(root->bus),
					root->bus->number, PCI_SLOT(root->devfn),
					PCI_FUNC(root->devfn));

	ret = pcie_capability_read_word(root, PCI_EXP_LNKCTL, &linkctl);
	if (ret)
		return -EINVAL;

	printk("%s == linkctl 0x%x\n", __func__, linkctl);

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

static void dfl_fpga_reload_rescan_pci_bus(void)
{
	struct pci_bus *b = NULL;

	printk("%s== rescan pci bus==\n", __func__);

	pci_lock_rescan_remove();
	while ((b = pci_find_next_bus(b)) != NULL)
		pci_rescan_bus(b);
	pci_unlock_rescan_remove();
}

static int dfl_fpga_reload_remove(struct pci_dev *root)
{
	printk("%s== remove FP0 and whole devices under root==\n", __func__);

	/* remove FP0 PCI dev */
	//pci_disable_pcie_error_reporting(pcidev);
	pci_stop_and_remove_bus_device_locked(root);

	return 0;
}

static ssize_t available_images_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct dfl_fpga_reload *dfl_reload = to_dfl_fpga_reload(dev);
	struct dfl_fpga_trigger *trigger = &dfl_reload->trigger;
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

static ssize_t reload_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
	struct dfl_fpga_reload *dfl_reload = to_dfl_fpga_reload(dev);
	struct pci_dev *pcidev, *root;
	struct dfl_fpga_trigger *trigger = &dfl_reload->trigger;
	int ret = -EINVAL;

	if (!dfl_reload->ops || !dfl_reload->priv ||
			!trigger->ops || !trigger->priv)
		return -EINVAL;

	pcidev = dfl_reload->priv;

	root = pcie_find_root_port(pcidev);
	if (!root)
		return -EINVAL;

	mutex_lock(&dfl_reload->lock);

	/* 1. remove all non-reserved devices */
	if (dfl_reload->ops->prepare)
		dfl_reload->ops->prepare(dfl_reload);

	/* 2. trigger BMC reload */
	if (trigger->ops->image_trigger)
		ret = trigger->ops->image_trigger(trigger, buf);

	/* 3. disable pci root hub link */
	ret = dfl_fpga_disable_pcie_link(root, true);
	if (ret)
		return -EINVAL;

	/* 4. remove reserved device and the whole PCI device under root devices*/
	dfl_fpga_reload_remove(root);

	/* 5. Wait for FPGA/BMC reload done. eg, 10s */
	if (!ret)
		mdelay(10*1000);

	/* 6. enable pci root hub link */
	ret = dfl_fpga_disable_pcie_link(root, false);
	if (ret)
		return -EINVAL;

	/* 7. rescan the PCI bus*/
	dfl_fpga_reload_rescan_pci_bus();

	mutex_unlock(&dfl_reload->lock);

	return ret ? : count;
}

static DEVICE_ATTR_RO(available_images);
static DEVICE_ATTR_WO(reload);

static struct attribute *dfl_fpga_reload_attrs[] = {
	&dev_attr_available_images.attr,
	&dev_attr_reload.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dfl_fpga_reload);

struct dfl_fpga_trigger *
dfl_fpga_reload_trigger_register(struct module *module,
                const struct dfl_fpga_trigger_ops *ops, void *priv)
{
	struct dfl_fpga_trigger *trigger = &dfl_reload->trigger;

	if (!ops){
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	printk("%s =====\n", __func__);

	//if (!try_module_get(module))
	//	return ERR_PTR(-EFAULT);

	trigger->priv = priv;
	trigger->ops = ops;

	return trigger;
}
EXPORT_SYMBOL_GPL(dfl_fpga_reload_trigger_register);

void dfl_fpga_reload_trigger_unregister(struct dfl_fpga_trigger *trigger)
{

	trigger->priv = NULL;
	trigger->ops = NULL;
	//module_put(dfl_reload->module);
}
EXPORT_SYMBOL_GPL(dfl_fpga_reload_trigger_unregister);

struct dfl_fpga_reload *
dfl_fpga_reload_dev_register(struct module *module,
                const struct dfl_fpga_reload_ops *ops, void *priv)
{
	if (!ops){
		dev_err(&dfl_reload->dev, "Attempt to register without all required ops\n");
		return ERR_PTR(-EINVAL);
	}

	//if (!try_module_get(module))
	//	return ERR_PTR(-EFAULT);

	dfl_reload->priv = priv;
	dfl_reload->ops = ops;
	dfl_reload->module = module;

	return dfl_reload;
}
EXPORT_SYMBOL_GPL(dfl_fpga_reload_dev_register);

void dfl_fpga_reload_dev_unregister(struct dfl_fpga_reload *dfl_reload)
{
	dfl_reload->priv = NULL;
	dfl_reload->ops = NULL;
	//module_put(dfl_reload->module);
}
EXPORT_SYMBOL_GPL(dfl_fpga_reload_dev_unregister);

static void dfl_fpga_reload_dev_release(struct device *dev)
{
	struct dfl_fpga_reload *dfl_reload = to_dfl_fpga_reload(dev);

	xa_erase(&dfl_fpga_reload_xa, dfl_reload->dev.id);
}

static int __init dfl_fpga_reload_init(void)
{
	int ret;

	dfl_fpga_reload_class = class_create(THIS_MODULE, "dfl_fpga_reload");
	if (IS_ERR(dfl_fpga_reload_class))
		return PTR_ERR(dfl_fpga_reload_class);

	dfl_fpga_reload_class->dev_groups = dfl_fpga_reload_groups;
	dfl_fpga_reload_class->dev_release = dfl_fpga_reload_dev_release;

	dfl_reload = kzalloc(sizeof(*dfl_reload), GFP_KERNEL);
	if (!dfl_reload) {
		ret = -ENOMEM;
		goto free_class;
	}

	ret = xa_alloc(&dfl_fpga_reload_xa, &dfl_reload->dev.id,
			dfl_reload, DFL_FPGA_RELOAD_XA_LIMIT, GFP_KERNEL);
	if (ret)
		goto error_kfree;

	dfl_reload->dev.class = dfl_fpga_reload_class;
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
	xa_erase(&dfl_fpga_reload_xa, dfl_reload->dev.id);

error_kfree:
	kfree(dfl_reload);
free_class:
	class_destroy(dfl_fpga_reload_class);
	
	return ret;
}

static void __exit dfl_fpga_reload_exit(void)
{
	device_unregister(&dfl_reload->dev);

	class_destroy(dfl_fpga_reload_class);

	kfree(dfl_reload);
}

module_init(dfl_fpga_reload_init);
module_exit(dfl_fpga_reload_exit);

MODULE_DESCRIPTION("DFL FPGA reload Support");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
