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

struct dfl_image_reload;

/**
 * struct dfl_image_reload_ops - image reload specific operations
 * @prepare: Required: Prepare image reload, remove non-reserved devices
 */
struct dfl_image_reload_ops {
	int (*prepare)(struct dfl_image_reload *dfl_reload);
};

/**
 * struct dfl_image_reload - represent a dfl image reload instance
 *
 * @dev: generic device interface.
 * @lock: mutex to protect reload data
 * @priv: private data for dfl_image_reload
 * @ops: ops of this dfl_image_reload
 */
struct dfl_image_reload {
	struct device dev;
	struct mutex lock; /* protect data structure contents */
	void *priv;
	const struct dfl_image_reload_ops *ops;
};

struct dfl_image_reload *
dfl_image_reload_dev_register(const struct dfl_image_reload_ops *ops, void *priv);
void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload);

#endif

