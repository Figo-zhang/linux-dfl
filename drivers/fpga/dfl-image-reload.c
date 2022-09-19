#include <linux/dfl.h>
#include <linux/pci.h>
#include <linux/fpga-dfl.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include "dfl.h"
#include "dfl-image-reload.h"

#define DFL_FPGA_RELOAD_XA_LIMIT  XA_LIMIT(0, INT_MAX)
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

static ssize_t reload_store(struct device *dev,
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
static DEVICE_ATTR_WO(reload);

static struct attribute *dfl_image_reload_attrs[] = {
	&dev_attr_available_images.attr,
	&dev_attr_reload.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dfl_image_reload);

struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops, void *priv)
{
	struct dfl_image_trigger *trigger = &dfl_reload->trigger;

	if (!ops){
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
	if (!ops){
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
			dfl_reload, DFL_FPGA_RELOAD_XA_LIMIT, GFP_KERNEL);
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

MODULE_DESCRIPTION("DFL FPGA Image Reload Support");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
