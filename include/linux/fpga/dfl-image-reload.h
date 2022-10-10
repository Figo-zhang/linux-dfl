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
 */
struct dfl_image_trigger {
	const struct dfl_image_trigger_ops *ops;
	void *priv;
};

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
 * @trigger: dfl_image_trigger instance
 */
struct dfl_image_reload {
	struct device dev;
	struct pci_dev *fpga_dev;
	struct mutex lock; /* protect data structure contents */
	void *priv;
	const struct dfl_image_reload_ops *ops;
	struct dfl_image_trigger trigger;
};

/* Timeout (10s) for image reload */
#define RELOAD_TIMEOUT_MS  (10 * 1000)

struct dfl_image_reload *
pcie_fpga_reload_register(struct pci_dev *fpga_dev, const struct dfl_image_reload_ops *ops);
void pcie_fpga_reload_unregister(struct dfl_image_reload *reload);

#endif

