/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver Header File for DFL FPGA Image Reload Hotplug Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#ifndef _LINUX_DFL_HP_IMAGE_RELOAD_H
#define _LINUX_DFL_HP_IMAGE_RELOAD_H

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>

struct dfl_image_reload;

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
 * @IMAGE_RELOAD_FAIL: image reload failed
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
 * @lock: mutex to protect reload data
 * @is_registered: register status
 * @priv: private data for dfl_image_reload
 * @ops: ops of this dfl_image_reload
 * @state: the status of image reload
 * @name: name of the image reload device.
 */
struct dfl_image_reload {
	struct mutex lock; /* protect data structure contents */
	bool is_registered;
	void *priv;
	const struct dfl_image_reload_ops *ops;
	enum image_reload_states state;
	const char *name;
};

#define to_dfl_reload(d) container_of(d, struct dfl_image_reload, slot)

struct dfl_image_reload *
dfl_hp_register_image_reload(const char *name,
			     const struct dfl_image_reload_ops *ops, void *priv);
void dfl_hp_unregister_image_reload(struct dfl_image_reload *dfl_reload);

#endif
