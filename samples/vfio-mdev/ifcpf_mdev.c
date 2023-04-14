#ifdef KEEP_SIMPLE_COPYRIGHT
/* Copyright (c) 2020, Intel Corporation. */
#endif

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/hashtable.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/bitmap.h>
#include <linux/socket.h>
#include <linux/stringify.h>
#include <linux/vfio.h>
#include <linux/mdev.h>

#define DRV_VERSION     "@VERSION@"
#define DRV_SUMMARY     "@SUMMARY@"
static const char ifcpf_mdev_driver_version[] = DRV_VERSION;
static const char ifcpf_mdev_driver_string[] = DRV_SUMMARY;
static const char ifcpf_mdev_copyright[] = "@COPYRIGHT@";

MODULE_AUTHOR("Intel Corporation, <linux.nics@intel.com>");
MODULE_DESCRIPTION(DRV_SUMMARY);
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);

#define VFIO_PCI_OFFSET_SHIFT   40

#define MTTY_STRING_LEN		16

#define MTTY_CONFIG_SPACE_SIZE  0x100
#define MTTY_IO_BAR_SIZE        0x8
#define MTTY_MMIO_BAR_SIZE      0x100000

#define STORE_LE16(addr, val)   (*(u16 *)addr = val)
#define STORE_LE32(addr, val)   (*(u32 *)addr = val)

#define MAX_FIFO_SIZE   16

#define MTTY_VFIO_PCI_OFFSET_SHIFT   40

#define MTTY_VFIO_PCI_OFFSET_TO_INDEX(off)   (off >> MTTY_VFIO_PCI_OFFSET_SHIFT)
#define MTTY_VFIO_PCI_INDEX_TO_OFFSET(index) \
				((u64)(index) << MTTY_VFIO_PCI_OFFSET_SHIFT)
#define MTTY_VFIO_PCI_OFFSET_MASK    \
				(((u64)(1) << MTTY_VFIO_PCI_OFFSET_SHIFT) - 1)

struct mdev_region_info {
	u64 start;
	u64 phys_start;
	u32 size;
	u64 vfio_offset;
};

/* State of each mdev device */
struct mdev_state {
	struct vfio_device vdev;
	struct pci_dev *pdev;
	u8 vconfig[MTTY_CONFIG_SPACE_SIZE];
	struct mutex ops_lock;
	struct mdev_device *mdev;
	struct mdev_region_info region_info[VFIO_PCI_NUM_REGIONS];
	u32 bar_mask[VFIO_PCI_NUM_REGIONS];
	struct vfio_device_info dev_info;
	struct list_head list;
};
static LIST_HEAD(ifcpf_mdev_list);
static int ifcpf_mdev_count;
static void ifcpf_mdev_create_config_space(struct mdev_state *mdev_state)
{
	u8 cfg_space_data[MTTY_CONFIG_SPACE_SIZE] = {
		0x86, 0x80, 0x86, 0x80, /* vendor id, device id */
		0x46, 0x01, 0x10, 0x00, /* cmd reg, status reg */
		0x00, 0x00, 0x00, 0x02, /* class */
		0x08, 0x00, 0x80, 0x00, /* BIST */
		0x0c, 0x00, 0x40, 0xff, /* base reg 0 */
		0x3f, 0x38, 0x00, 0x00, /* base reg 1 */
		0x0c, 0x40, 0x50, 0xff, /* base reg 2 */
		0x3f, 0x38, 0x00, 0x00, /* base reg 3 */
		0x0c, 0x00, 0xc0, 0xfe, /* base reg 4 */
		0x3f, 0x38, 0x00, 0x00, /* base reg 5 */
		0x00, 0x00, 0x00, 0x00, /* cardbus */
		/* subsystem vendor id, subsystem device id */
		0x86, 0x80, 0xfe, 0x15,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00};
	memcpy(mdev_state->vconfig, cfg_space_data, MTTY_CONFIG_SPACE_SIZE);
	mdev_state->bar_mask[0] = ~(MTTY_IO_BAR_SIZE) + 1;
	mdev_state->bar_mask[1] = ~(MTTY_IO_BAR_SIZE) + 1;
}

static void handle_pci_cfg_write(struct mdev_state *mdev_state, u16 offset,
				 char *buf, u32 count)
{
	u32 cfg_addr, bar_mask, bar_index = 0;

	pr_info(">>>>>>>>>>>>>>>>>%s\n", __func__);

	switch (offset) {
	case 0x04: /* device control */
	case 0x06: /* device status */
		/* do nothing */
		break;
	case 0x3c:  /* interrupt line */
		pr_info("PCI write Interrupt Line @0x%x of %d bytes not handled\n",
			offset, count);
		break;
	case 0x3d:
		/*
		 * Interrupt Pin is hardwired to INTA.
		 * This field is write protected by hardware
		 */
		pr_info("PCI write Interrupt Pin @0x%x of %d bytes not handled\n",
			offset, count);
		break;
	case 0x10:  /* BAR0 */
	case 0x14:  /* BAR1 */
		if (offset == 0x10)
			bar_index = 0;
		else if (offset == 0x14)
			bar_index = 1;

		cfg_addr = *(u32 *)buf;
		pr_info("BAR%d addr 0x%x\n", bar_index, cfg_addr);

		if (cfg_addr == 0xffffffff) {
			bar_mask = mdev_state->bar_mask[bar_index];
			cfg_addr = (cfg_addr & bar_mask);
		}

		cfg_addr |= (mdev_state->vconfig[offset] & 0x3ul);
		STORE_LE32(&mdev_state->vconfig[offset], cfg_addr);
		break;
	case 0x18:  /* BAR2 */
	case 0x1c:  /* BAR3 */
	case 0x20:  /* BAR4 */
		STORE_LE32(&mdev_state->vconfig[offset], 0);
		break;
	default:
		pr_info("PCI config write @0x%x of %d bytes not handled\n",
			offset, count);
		break;
	}
}

static void handle_bar_write(unsigned int index, struct mdev_state *mdev_state,
				u16 offset, char *buf, u32 count)
{
	pr_info("PCI BAR write @0x%x of %d bytes not handled\n",
		offset, count);
}

static void handle_bar_read(unsigned int index, struct mdev_state *mdev_state,
			    u16 offset, char *buf, u32 count)
{
	pr_info("PCI BAR read @0x%x of %d bytes not handled\n",
		offset, count);
}

static void mdev_read_base(struct mdev_state *mdev_state)
{
	int index, pos;
	u32 start_lo, start_hi;
	u32 mem_type;

	pos = PCI_BASE_ADDRESS_0;

	for (index = 0; index <= VFIO_PCI_BAR5_REGION_INDEX; index++) {

		if (!mdev_state->region_info[index].size)
			continue;

		start_lo = (*(u32 *)(mdev_state->vconfig + pos)) &
			PCI_BASE_ADDRESS_MEM_MASK;
		mem_type = (*(u32 *)(mdev_state->vconfig + pos)) &
			PCI_BASE_ADDRESS_MEM_TYPE_MASK;

		switch (mem_type) {
		case PCI_BASE_ADDRESS_MEM_TYPE_64:
			start_hi = (*(u32 *)(mdev_state->vconfig + pos + 4));
			pos += 4;
			break;
		case PCI_BASE_ADDRESS_MEM_TYPE_32:
		case PCI_BASE_ADDRESS_MEM_TYPE_1M:
			/* 1M mem BAR treated as 32-bit BAR */
		default:
			/* mem unknown type treated as 32-bit BAR */
			start_hi = 0;
			break;
		}
		pos += 4;
		mdev_state->region_info[index].start = ((u64)start_hi << 32) |
							start_lo;
	}
}

static ssize_t mdev_access(struct mdev_state *mdev_state, char *buf, size_t count,
			   loff_t pos, bool is_write)
{
	unsigned int index;
	loff_t offset;
	int ret = 0;

	if (!buf)
		return -EINVAL;

	mutex_lock(&mdev_state->ops_lock);

	index = MTTY_VFIO_PCI_OFFSET_TO_INDEX(pos);
	offset = pos & MTTY_VFIO_PCI_OFFSET_MASK;

	/* Checking offset value fall in configure mdev_state->vconfig size */
	if (offset >= MTTY_CONFIG_SPACE_SIZE) {
		pr_info("%s offset %llu more than allowed size %d\n",
			__func__, offset, MTTY_CONFIG_SPACE_SIZE);

		goto accessfailed;
	}

	pr_info("%s index %d offset %llu\n", __func__, index, offset);
	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:

		pr_info("%s: PCI config space %s at offset 0x%llx\n",
			 __func__, is_write ? "write" : "read", offset);

		if (is_write)
			handle_pci_cfg_write(mdev_state, offset, buf, count);
		else
			memcpy(buf, (mdev_state->vconfig + offset), count);

		break;

	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		if (!mdev_state->region_info[index].start)
			mdev_read_base(mdev_state);

		if (is_write) {
			pr_info("%s: BAR%d  WR @0x%llx\n",
				__func__, index, offset);

			handle_bar_write(index, mdev_state, offset, buf, count);
		} else {
			handle_bar_read(index, mdev_state, offset, buf, count);

			pr_info("%s: BAR%d  RD @0x%llx\n",
				__func__, index, offset);
		}
		break;

	default:
		ret = -1;
		goto accessfailed;
	}

	ret = count;


accessfailed:
	mutex_unlock(&mdev_state->ops_lock);

	return ret;
}

int ifcpf_reset(struct mdev_state *mdev_state)
{
	return 0;
}

ssize_t ifcpf_mdev_read(struct vfio_device *vdev, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			ret =  mdev_access(mdev_state, (char *)&val, sizeof(val),
					   *ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			ret = mdev_access(mdev_state, (char *)&val, sizeof(val),
					  *ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 2;
		} else {
			u8 val;

			ret = mdev_access(mdev_state, (char *)&val, sizeof(val),
					  *ppos, false);
			if (ret <= 0)
				goto read_err;

			if (copy_to_user(buf, &val, sizeof(val)))
				goto read_err;

			filled = 1;
		}

		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	return done;

read_err:
	return -EFAULT;
}

ssize_t ifcpf_mdev_write(struct vfio_device *vdev, const char __user *buf,
		   size_t count, loff_t *ppos)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);
	unsigned int done = 0;
	int ret;

	while (count) {
		size_t filled;

		if (count >= 4 && !(*ppos % 4)) {
			u32 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = mdev_access(mdev_state, (char *)&val, sizeof(val),
					  *ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 4;
		} else if (count >= 2 && !(*ppos % 2)) {
			u16 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = mdev_access(mdev_state, (char *)&val, sizeof(val),
					  *ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 2;
		} else {
			u8 val;

			if (copy_from_user(&val, buf, sizeof(val)))
				goto write_err;

			ret = mdev_access(mdev_state, (char *)&val, sizeof(val),
					  *ppos, true);
			if (ret <= 0)
				goto write_err;

			filled = 1;
		}
		count -= filled;
		done += filled;
		*ppos += filled;
		buf += filled;
	}

	return done;
write_err:
	return -EFAULT;
}

int ifcpf_get_region_info(struct mdev_state *mdev_state,
			 struct vfio_region_info *region_info,
			 u16 *cap_type_id, void **cap_type)
{
	unsigned int size = 0;
	struct pci_dev *pdev;
	u32 bar_index;

	pdev = mdev_state->pdev;

	bar_index = region_info->index;
	if (bar_index >= VFIO_PCI_NUM_REGIONS)
		return -EINVAL;

	mutex_lock(&mdev_state->ops_lock);

	switch (bar_index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		size = MTTY_CONFIG_SPACE_SIZE;
		pr_info("%s get config region, size is %x\n", __func__, size);
		region_info->flags = VFIO_REGION_INFO_FLAG_READ |
			VFIO_REGION_INFO_FLAG_WRITE;
		break;
	case VFIO_PCI_BAR0_REGION_INDEX:
		size = pci_resource_len(pdev, 4);
		pr_info("%s get bar 0, size is %x\n", __func__, size);
		region_info->flags = VFIO_REGION_INFO_FLAG_READ |
			VFIO_REGION_INFO_FLAG_WRITE |
			VFIO_REGION_INFO_FLAG_MMAP;
		break;
	default:
		size = 0;
		break;
	}

	mdev_state->region_info[bar_index].size = size;
	mdev_state->region_info[bar_index].vfio_offset =
		MTTY_VFIO_PCI_INDEX_TO_OFFSET(bar_index);

	region_info->size = size;
	region_info->offset = MTTY_VFIO_PCI_INDEX_TO_OFFSET(bar_index);
	/*region_info->flags = VFIO_REGION_INFO_FLAG_READ |
		VFIO_REGION_INFO_FLAG_WRITE;*/
	mutex_unlock(&mdev_state->ops_lock);
	return 0;
}

int ifcpf_get_device_info(struct vfio_device_info *dev_info)
{
	pr_info(">>>>>>>>>>>>>>>>>%s\n", __func__);

	dev_info->flags = VFIO_DEVICE_FLAGS_PCI;
	dev_info->num_regions = VFIO_PCI_NUM_REGIONS;
	dev_info->num_irqs = VFIO_PCI_NUM_IRQS;

	return 0;
}
int ifcpf_get_irq_info(struct vfio_irq_info *irq_info)
{
	if (irq_info->index != VFIO_PCI_MSIX_IRQ_INDEX)
		return -ENOTSUPP;

	irq_info->flags = VFIO_IRQ_INFO_EVENTFD;
	irq_info->count = 1;

	return 0;
}

static long ifcpf_mdev_ioctl(struct vfio_device *vdev, unsigned int cmd,
			unsigned long arg)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);
	int ret = 0;
	unsigned long minsz;

	pr_info(">>>>>>>>>>>>>>>>>%s\n", __func__);

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO:
	{
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		ret = ifcpf_get_device_info(&info);
		if (ret)
			return ret;

		memcpy(&mdev_state->dev_info, &info, sizeof(info));

		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;

		return 0;
	}
	case VFIO_DEVICE_GET_REGION_INFO:
	{
		struct vfio_region_info info;
		u16 cap_type_id = 0;
		void *cap_type = NULL;

		minsz = offsetofend(struct vfio_region_info, offset);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		ret = ifcpf_get_region_info(mdev_state, &info, &cap_type_id,
					   &cap_type);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;

		return 0;
	}
	case VFIO_DEVICE_GET_IRQ_INFO:
	{
		struct vfio_irq_info info;

		minsz = offsetofend(struct vfio_irq_info, count);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if ((info.argsz < minsz) ||
		    (info.index >= mdev_state->dev_info.num_irqs))
			return -EINVAL;

		ret = ifcpf_get_irq_info(&info);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;

		return 0;
	}
	case VFIO_DEVICE_RESET:
		return ifcpf_reset(mdev_state);
	}
	return -ENOTTY;
}

int ifcpf_mdev_mmap(struct vfio_device *vdev, struct vm_area_struct *vma)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);
	struct pci_dev *pdev;
	u64 phys_len, req_len, pgoff, req_start;

	pdev = mdev_state->pdev;

	/* index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT); */

	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;
	/*if (index >= VFIO_PCI_BAR1_REGION_INDEX)
		return -EINVAL;*/

	phys_len = PAGE_ALIGN(pci_resource_len(pdev, 4));
	req_len = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	if (req_start + req_len > phys_len)
		return -EINVAL;

	vma->vm_private_data = pdev;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = (pci_resource_start(pdev, 4) >> PAGE_SHIFT) + pgoff;

	pr_info("%s phys_len %llu req_len %llu\n", __func__,
		phys_len, req_len);
	return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			       req_len, vma->vm_page_prot);
}

int ifcpf_mdev_open(struct vfio_device *vdev)
{
	pr_info("%s\n", __func__);
	return 0;
}

void ifcpf_mdev_close(struct vfio_device *vdev)
{
	pr_info("%s\n", __func__);
}

const struct attribute_group *mdev_dev_groups[] = {
	NULL,
};

static ssize_t
name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const char *name_str = "virtio mdev";

	return sprintf(buf, "%s\n", name_str);
}

static DEVICE_ATTR_RO(name);

static ssize_t device_api_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%s\n", VFIO_DEVICE_API_PCI_STRING);
}
static DEVICE_ATTR_RO(device_api);

static ssize_t
available_instances_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", ifcpf_mdev_count);
}
static DEVICE_ATTR_RO(available_instances);

static struct attribute *mdev_types_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_device_api.attr,
	&dev_attr_available_instances.attr,
	NULL,
};

static struct attribute_group mdev_type_group1 = {
	.name  = "virtio_mdev",
	.attrs = mdev_types_attrs,
};

static const struct attribute_group *mdev_type_groups[] = {
	&mdev_type_group1,
	NULL,
};

static int ifcpf_init_dev(struct vfio_device *vdev)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);
	struct mdev_device *mdev = to_mdev_device(vdev->dev);
	struct pci_dev *pdev = mdev_state->pdev;
	int exist;

	if (!mdev)
		return -EINVAL;

	exist = 0;
	list_for_each_entry(mdev_state, &ifcpf_mdev_list, list) {
		if (mdev_state->pdev == pdev)
			exist = 1;
	}

	if (exist)
		return 0;

	mutex_init(&mdev_state->ops_lock);
	mdev_state->mdev = mdev;
	mdev_state->pdev = pdev;
	list_add(&mdev_state->list, &ifcpf_mdev_list);

	ifcpf_mdev_create_config_space(mdev_state);

	ifcpf_mdev_count++;

	return 0;
}

static void ifcpf_release_dev(struct vfio_device *vdev)
{
	struct mdev_state *mdev_state =
		container_of(vdev, struct mdev_state, vdev);

	list_del(&mdev_state->list);
	kfree(mdev_state);

	ifcpf_mdev_count--;
}

static const struct vfio_device_ops ifcpf_dev_ops = {
	.name = "vfio-ifcpf",
	.init = ifcpf_init_dev,
	.release = ifcpf_release_dev,
	.open_device = ifcpf_mdev_open,
	.close_device = ifcpf_mdev_close,
	.read = ifcpf_mdev_read,
	.write = ifcpf_mdev_write,
	.ioctl = ifcpf_mdev_ioctl,
	.mmap  = ifcpf_mdev_mmap,
};

static int ifcpf_probe(struct mdev_device *mdev)
{
	struct mdev_state *mdev_state;
	int ret;

	mdev_state = vfio_alloc_device(mdev_state, vdev, &mdev->dev,
				       &ifcpf_dev_ops);
	if (IS_ERR(mdev_state))
		return PTR_ERR(mdev_state);

	ret = vfio_register_emulated_iommu_dev(&mdev_state->vdev);
	if (ret)
		goto err_put_vdev;
	dev_set_drvdata(&mdev->dev, mdev_state);
	return 0;

err_put_vdev:
	vfio_put_device(&mdev_state->vdev);
	return ret;
}

static void ifcpf_remove(struct mdev_device *mdev)
{
	struct mdev_state *mdev_state = dev_get_drvdata(&mdev->dev);

	vfio_unregister_group_dev(&mdev_state->vdev);
	vfio_put_device(&mdev_state->vdev);
}

static struct ifcpf_type {
	struct mdev_type type;
	int nr_ports;
} ifcpf_types[1] = {
	{ .nr_ports = 1, .type.sysfs_name = "1",
	  .type.pretty_name = "ifcpf" },
};

static struct mdev_type *ifcpf_mdev_types[] = {
	&ifcpf_types[0].type,
};

static struct mdev_driver ifcpf_driver = {
	.device_api = VFIO_DEVICE_API_PCI_STRING,
	.driver = {
		.name = "ipcpf_mdev",
		.owner = THIS_MODULE,
		.mod_name = KBUILD_MODNAME,
		.dev_groups = mdev_type_groups,
	},
	.probe = ifcpf_probe,
	.remove	= ifcpf_remove,
};

static struct mdev_parent parent;

/**
 * ifcpf_mdev_init_module - Driver registration routine
 *
 * ifcpf_mdev_init_module is the first routine called when the driver is
 * loaded. All it does is register with the PCI subsystem.
 */
static int __init ifcpf_mdev_init_module(void)
{
	struct pci_dev *pdev;
	struct device *dev;
	int rc, exist;
	struct mdev_state *mstate;

	pr_info("%s - version %s\n",
		ifcpf_mdev_driver_string,
		ifcpf_mdev_driver_version);

	pr_info("%s\n", ifcpf_mdev_copyright);
	pr_info("ifcpf_dev: %s\n", __func__);

	//pdev = pci_get_device(0x1af4, 0x1041, NULL);
	pdev = pci_get_device(0x8086, 0xbcce, NULL);
	while (pdev) {
		pr_info("Successfully to find pci device\n");

		dev = &pdev->dev;
		exist = 0;
		list_for_each_entry(mstate, &ifcpf_mdev_list, list) {
			if (mstate->pdev == pdev)
				exist = 1;
		}
		if (!exist) {
			rc = mdev_register_driver(&ifcpf_driver);
			if (rc)
				pr_err("Failed to register mdev driver\n");

			rc = mdev_register_parent(&parent, dev, &ifcpf_driver,
				ifcpf_mdev_types, ARRAY_SIZE(ifcpf_mdev_types));
			if (rc) {
				mdev_unregister_driver(&ifcpf_driver);
				pr_err("Failed to register mdev device\n");
			} else {
				pr_info("%s\n register mdev success",
					__func__);
			}
		} else {
			pr_info(">>>>>>>>>>>>>>>>>%s ifcpf mdev exists\n",
				__func__);
		}

		//pdev = pci_get_device(0x1af4, 0x1041, pdev);
		pdev = pci_get_device(0x8086, 0xbcce, pdev);
	}

	return 0;
}
module_init(ifcpf_mdev_init_module);

/**
 * ifcpf_mdev_exit_module - Driver exit cleanup routine
 *
 * ifcpf_mdev_exit_module is called just before the driver is removed
 * from memory.
 */
static void __exit ifcpf_mdev_exit_module(void)
{
	struct pci_dev *pdev;

	//pdev = pci_get_device(0x1af4, 0x1041, NULL);
	pdev = pci_get_device(0x8086, 0xbcce, NULL);
	while (pdev) {
		mdev_unregister_parent(&parent);
		mdev_unregister_driver(&ifcpf_driver);
		pr_info("ifcpf_dev: Unloaded!\n");
		//pdev = pci_get_device(0x1af4, 0x1041, pdev);
		pdev = pci_get_device(0x8086, 0xbcce, pdev);
	}

}
module_exit(ifcpf_mdev_exit_module);
