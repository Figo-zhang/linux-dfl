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

/**
 * struct dfl_image_reload - represent a dfl image reload instance
 *
 * @dev: generic device interface.
 * @lock: mutex to protect reload data
 * @priv: private data for dfl_image_reload
 */
struct dfl_image_reload {
	struct device dev;
	struct mutex lock; /* protect data structure contents */
	void *priv;
};

#endif

