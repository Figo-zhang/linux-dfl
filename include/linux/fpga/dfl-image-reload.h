/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver Header File for DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2022 Intel Corporation, Inc.
 *
 */
#ifndef _LINUX_DFL_IMAGE_RELOAD_H
#define _LINUX_DFL_IMAGE_RELOAD_H

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>

struct dfl_image_reload;
struct dfl_image_trigger;

/**
 * struct dfl_image_trigger_ops - image trigger specific operations
 * @available_images: Required: available images for reload trigger
 * @image_trigger: Required: trigger the image reload on BMC
 */
struct dfl_image_trigger_ops {
	ssize_t (*available_images)(struct dfl_image_trigger *trigger, char *buf);
	int (*image_trigger)(struct dfl_image_trigger *trigger, const char *buf);
};

/**
 * struct dfl_image_trigger - represent a dfl image trigger instance
 *
 * @ops: ops of this dfl_image_trigger
 * @priv: private data for dfl_image_trigger
 * @parent: parent device of trigger
 * @is_registered: register status
 * @wait_time: seconds of wait time for image reload
 */
struct dfl_image_trigger {
	const struct dfl_image_trigger_ops *ops;
	void *priv;
	struct device *parent;
	bool is_registered;
	u32 wait_time;
};

/**
 * struct dfl_image_reload_ops - image reload specific operations
 * @reload_prepare: Required: Prepare image reload, remove non-reserved devices
 */
struct dfl_image_reload_ops {
        int (*reload_prepare)(struct dfl_image_reload *dfl_reload);
};

/**
 * enum image_reload_states - image reload states
 * @IMAGE_RELOAD_UNKNOWN: can't determine state
 * @IMAGE_RELOAD_RELOADING: doing the image reload
 * @IMAGE_RELOAD_DONE: image reload done
 * @IMAGE_RELOAD_FAIL: image reload failure
 */
enum image_reload_states {
	IMAGE_RELOAD_UNKNOWN,
	IMAGE_RELOAD_RELOADING,
	IMAGE_RELOAD_DONE,
	IMAGE_RELOAD_FAIL,
};

/**
 * struct dfl_image_reload - represent a dfl image reload instance
 *
 * @slot: structure registered with the PCI hotplug core
 * @lock: mutex to protect reload data
 * @is_registered: register status
 * @priv: private data for dfl_image_reload
 * @ops: ops of this dfl_image_reload
 * @state: the status of image reload
 * @trigger: dfl_image_trigger instance
 * @node: node in list of device list.
 */
struct dfl_image_reload {
	struct mutex lock; /* protect data structure contents */
	bool is_registered;
	void *priv;
	const struct dfl_image_reload_ops *ops;
	enum image_reload_states state;
	const char *name;
	struct dfl_image_trigger trigger;
};

/* default wait seconds for image reload */
#define RELOAD_DEFAULT_WAIT_SECS  10

#define SLOT_NAME_SIZE 10

#define to_dfl_reload(d) container_of(d, struct dfl_image_reload, slot)

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name,
			      const struct dfl_image_reload_ops *ops, void *priv);
void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload);
struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops,
				  struct device *parent, void *priv);
void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger);

#endif

