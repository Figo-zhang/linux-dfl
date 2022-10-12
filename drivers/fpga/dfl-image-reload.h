/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver Header File for DFL FPGA Image Reload Driver
 *
 * Copyright (C) 2019-2022 Intel Corporation, Inc.
 *
 */
#ifndef _LINUX_DFL_IMAGE_RELOAD_H
#define _LINUX_DFL_IMAGE_RELOAD_H

#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>

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
 * @is_registered: register status
 */
struct dfl_image_trigger {
	const struct dfl_image_trigger_ops *ops;
	void *priv;
	bool is_registered;
};

/**
 * struct dfl_image_reload - represent a dfl image reload instance
 *
 * @lock: mutex to protect reload data
 * @is_registered: register status
 * @priv: private data for dfl_image_reload
 * @mgr: fpga manager instance
 * @ops: ops of this dfl_image_reload
 * @trigger: dfl_image_trigger instance
 * @node: node in list of device list.
 */
struct dfl_image_reload {
	struct mutex lock; /* protect data structure contents */
	bool is_registered;
	void *priv;
	struct fpga_manager *mgr;
	struct dfl_image_trigger trigger;
	struct list_head node;
};

/* Timeout (10s) for image reload */
#define RELOAD_TIMEOUT_MS  (10 * 1000)

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name,
			      const struct fpga_manager_ops *ops, void *priv);
void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload);
struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops,
				  struct device *parent, void *priv);
void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger);

extern const struct attribute_group *dfl_reload_attr_groups[];

#endif

