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
	struct list_head node;
};

struct dfl_image_reload *
dfl_image_reload_dev_register(const char *name,
			      const struct fpga_manager_ops *ops, void *priv);
void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload);

#endif

