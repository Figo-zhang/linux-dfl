// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Card Manager Core
 * Borrowed from fpga-mgr.c
 *
 * Copyright (C) 2022 Intel Corporation. All rights reserved.
 *
 */
#include <linux/fpga/fpga-card.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/slab.h>

static DEFINE_IDA(fpga_card_ida);
static struct class *fpga_card_class;

struct fpga_card_devres {
	struct fpga_card *card;
};

static inline void fpga_card_remove(struct fpga_card *card)
{
	if (card->mops->card_remove)
		card->mops->card_remove(card);
}

static inline enum fpga_card_states fpga_card_state(struct fpga_card *card)
{
	if (card->mops->state)
		return  card->mops->state(card);
	return FPGA_CARD_STATE_UNKNOWN;
}

static const char * const state_str[] = {
	[FPGA_CARD_STATE_UNKNOWN] =		"unknown",
	[FPGA_CARD_STATE_RELOAD_PREPARE] =	"reload prepare",
	[FPGA_CARD_STATE_RELOAD_DONE] =	        "reload done",
};

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct fpga_card *card = to_fpga_card(dev);

	return sprintf(buf, "%s\n", card->name);
}

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct fpga_card *card = to_fpga_card(dev);

	return sprintf(buf, "%s\n", state_str[card->state]);
}

static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(state);

static struct attribute *fpga_card_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpga_card);

/**
 * fpga_card_prepare_image_reload - prepare the image reload
 * @card:	fpga card
 *
 * Return: 0 on success, negative error code otherwise.
 */
int fpga_card_prepare_image_reload(struct fpga_card *card)
{
	if (card->mops->reload_prepare)
		return card->mops->reload_prepare(card);

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_card_prepare_image_reload);

/**
 * fpga_card_lock - Lock FPGA card for exclusive use
 * @card:	fpga card
 *
 * Given a pointer to FPGA Card attempt to get the mutex.
 * The user should call fpga_card_lock() and verify that
 * it returns 0 before attempting to control the FPGA.
 *
 * Return: 0 for success or -EBUSY
 */
int fpga_card_lock(struct fpga_card *card)
{
	if (!mutex_trylock(&card->ref_mutex)) {
		dev_err(&card->dev, "FPGA card is in use.\n");
		return -EBUSY;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_card_lock);

/**
 * fpga_card_unlock - Unlock FPGA card
 * @card:	fpga card
 */
void fpga_card_unlock(struct fpga_card *card)
{
	mutex_unlock(&card->ref_mutex);
}
EXPORT_SYMBOL_GPL(fpga_card_unlock);

/**
 * fpga_card_register_full - create and register an FPGA Card device
 * @parent:	fpga card device from pdev
 * @info:	parameters for fpga card
 *
 * The caller of this function is responsible for calling fpga_card_unregister().
 * Using devm_fpga_card_register_full() instead is recommended.
 *
 * Return: pointer to struct fpga_card pointer or ERR_PTR()
 */
struct fpga_card *
fpga_card_register_full(struct device *parent, const struct fpga_card_info *info)
{
	const struct fpga_card_ops *mops = info->mops;
	struct fpga_card *card;
	int id, ret;

	if (!mops) {
		dev_err(parent, "Attempt to register without fpga_card_ops\n");
		return ERR_PTR(-EINVAL);
	}

	if (!info->name || !strlen(info->name)) {
		dev_err(parent, "Attempt to register with no name!\n");
		return ERR_PTR(-EINVAL);
	}

	card = kzalloc(sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc(&fpga_card_ida, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto error_kfree;
	}

	mutex_init(&card->ref_mutex);

	card->name = info->name;
	card->mops = info->mops;
	card->priv = info->priv;

	card->dev.class = fpga_card_class;
	card->dev.groups = mops->groups;
	card->dev.parent = parent;
	card->dev.of_node = parent->of_node;
	card->dev.id = id;

	ret = dev_set_name(&card->dev, "card%d", id);
	if (ret)
		goto error_device;

	/*
	 * Initialize framework state by requesting low level driver read state
	 * from device.
	 */
	card->state = fpga_card_state(card);

	ret = device_register(&card->dev);
	if (ret) {
		put_device(&card->dev);
		return ERR_PTR(ret);
	}

	return card;

error_device:
	ida_free(&fpga_card_ida, id);
error_kfree:
	kfree(card);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fpga_card_register_full);

/**
 * fpga_card_register - create and register an FPGA Card device
 * @parent:	fpga card device from pdev
 * @name:	fpga card name
 * @mops:	pointer to structure of fpga card ops
 * @priv:	fpga card private data
 *
 * The caller of this function is responsible for calling fpga_card_unregister().
 * Using devm_fpga_card_register() instead is recommended. This simple
 * version of the register function should be sufficient for most users. The
 * fpga_card_register_full() function is available for users that need to pass
 * additional, optional parameters.
 *
 * Return: pointer to struct fpga_card pointer or ERR_PTR()
 */
struct fpga_card *
fpga_card_register(struct device *parent, const char *name,
		   const struct fpga_card_ops *mops, void *priv)
{
	struct fpga_card_info info = { 0 };

	info.name = name;
	info.mops = mops;
	info.priv = priv;

	return fpga_card_register_full(parent, &info);
}
EXPORT_SYMBOL_GPL(fpga_card_register);

/**
 * fpga_card_unregister - unregister an FPGA Card
 * @card: fpga card struct
 *
 * This function is intended for use in an FPGA card driver's remove function.
 */
void fpga_card_unregister(struct fpga_card *card)
{
	dev_info(&card->dev, "%s %s\n", __func__, card->name);

	/*
	 * If the low level driver provides a method for putting fpga into
	 * a desired state upon unregister, do it.
	 */
	fpga_card_remove(card);

	device_unregister(&card->dev);
}
EXPORT_SYMBOL_GPL(fpga_card_unregister);

static void devm_fpga_card_unregister(struct device *dev, void *res)
{
	struct fpga_card_devres *dr = res;

	fpga_card_unregister(dr->card);
}

/**
 * devm_fpga_card_register_full - resource managed variant of fpga_card_register()
 * @parent:	fpga card device from pdev
 * @info:	parameters for fpga card
 *
 * Return:  fpga card pointer on success, negative error code otherwise.
 *
 * This is the devres variant of fpga_card_register_full() for which the unregister
 * function will be called automatically when the managing device is detached.
 */
struct fpga_card *
devm_fpga_card_register_full(struct device *parent, const struct fpga_card_info *info)
{
	struct fpga_card_devres *dr;
	struct fpga_card *card;

	dr = devres_alloc(devm_fpga_card_unregister, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return ERR_PTR(-ENOMEM);

	card = fpga_card_register_full(parent, info);
	if (IS_ERR(card)) {
		devres_free(dr);
		return card;
	}

	dr->card = card;
	devres_add(parent, dr);

	return card;
}
EXPORT_SYMBOL_GPL(devm_fpga_card_register_full);

/**
 * devm_fpga_card_register - resource managed variant of fpga_card_register()
 * @parent:	fpga card device from pdev
 * @name:	fpga card name
 * @mops:	pointer to structure of fpga card ops
 * @priv:	fpga card private data
 *
 * Return:  fpga card pointer on success, negative error code otherwise.
 *
 * This is the devres variant of fpga_card_register() for which the
 * unregister function will be called automatically when the managing
 * device is detached.
 */
struct fpga_card *
devm_fpga_card_register(struct device *parent, const char *name,
			const struct fpga_card_ops *mops, void *priv)
{
	struct fpga_card_info info = { 0 };

	info.name = name;
	info.mops = mops;
	info.priv = priv;

	return devm_fpga_card_register_full(parent, &info);
}
EXPORT_SYMBOL_GPL(devm_fpga_card_register);

static void fpga_card_dev_release(struct device *dev)
{
	struct fpga_card *card = to_fpga_card(dev);

	ida_free(&fpga_card_ida, card->dev.id);
	kfree(card);
}

static int __init fpga_card_class_init(void)
{
	pr_info("FPGA Card manager framework\n");

	fpga_card_class = class_create(THIS_MODULE, "fpga_card");
	if (IS_ERR(fpga_card_class))
		return PTR_ERR(fpga_card_class);

	fpga_card_class->dev_groups = fpga_card_groups;
	fpga_card_class->dev_release = fpga_card_dev_release;

	return 0;
}

static void __exit fpga_card_class_exit(void)
{
	class_destroy(fpga_card_class);
	ida_destroy(&fpga_card_ida);
}

MODULE_AUTHOR("Tianfei Zhang <tianfei.zhang@intel.com>");
MODULE_DESCRIPTION("FPGA Card manager framework");
MODULE_LICENSE("GPL");

subsys_initcall(fpga_card_class_init);
module_exit(fpga_card_class_exit);
