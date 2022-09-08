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

static void dfl_fpga_reload_rescan_pci_bus(void)
{
	struct pci_bus *b = NULL;

	printk("%s== rescan pci bus==\n", __func__);

	pci_lock_rescan_remove();
	while ((b = pci_find_next_bus(b)) != NULL)
		pci_rescan_bus(b);
	pci_unlock_rescan_remove();
}

static int dfl_fpga_reload_remove(struct dfl_fpga_reload *dfl_reload)
{
	struct pci_dev *pcidev = dfl_reload->priv;

	printk("%s== remove FP0==\n", __func__);

	/* remove FP0 PCI dev */
	pci_stop_and_remove_bus_device_locked(pcidev);

	return 0;
}

static ssize_t available_images_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t reload_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
	struct dfl_fpga_reload *dfl_reload = to_dfl_fpga_reload(dev);

	if (!dfl_reload->ops || !dfl_reload->priv)
		return -EINVAL;

	mutex_lock(&dfl_reload->lock);

	if (dfl_reload->ops->prepare)
		dfl_reload->ops->prepare(dfl_reload);

	dfl_fpga_reload_remove(dfl_reload);

	dfl_fpga_reload_rescan_pci_bus();

	mutex_unlock(&dfl_reload->lock);

	return count;
}

static DEVICE_ATTR_RO(available_images);
static DEVICE_ATTR_WO(reload);

static struct attribute *dfl_fpga_reload_attrs[] = {
	&dev_attr_available_images.attr,
	&dev_attr_reload.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dfl_fpga_reload);

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
