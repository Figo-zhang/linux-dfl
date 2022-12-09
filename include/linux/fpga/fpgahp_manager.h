/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver Header File for FPGA PCI Hotplug Driver
 *
 * Copyright (C) 2022 Intel Corporation
 *
 */
#ifndef _LINUX_FPGAHP_MANAGER_H
#define _LINUX_FPGAHP_MANAGER_H

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>

struct fpgahp_manager;
struct fpgahp_bmc_device;

/**
 * struct fpgahp_bmc_ops - fpga hotplug BMC specific operations
 * @available_images: Required: available images for fpgahp trigger
 * @image_trigger: Required: trigger the image reload on BMC
 */
struct fpgahp_bmc_ops {
	ssize_t (*available_images)(struct fpgahp_bmc_device *bmc, char *buf);
	int (*image_trigger)(struct fpgahp_bmc_device *bmc, const char *buf,
			     u32 *wait_time_msec);
};

/**
 * struct fpgahp_bmc_device - represent a fpga hotplug BMC device
 *
 * @ops: ops of this fpgahp_bmc_device
 * @priv: private data for fpgahp_bmc_device
 * @parent: parent device of fpgahp_bmc_device
 * @is_registered: register status
 */
struct fpgahp_bmc_device {
	const struct fpgahp_bmc_ops *ops;
	void *priv;
	struct device *parent;
	bool is_registered;
};

/**
 * struct fpgahp_manager_ops - fpgahp manager specific operations
 * @hotplug_prepare: Required: hotplug prepare like remove non-reserved devices
 *                   below the PCI device.
 */
struct fpgahp_manager_ops {
	int (*hotplug_prepare)(struct fpgahp_manager *mgr);
};

/**
 * enum fpgahp_manager_states - fpga hotplug states
 * @FPGAHP_MGR_UNKNOWN: can't determine state
 * @FPGAHP_MGR_LOADING: image loading
 * @FPGAHP_MGR_LOAD_DONE: image load done
 * @FPGAHP_MGR_HP_FAIL: hotplug failed
 */
enum fpgahp_manager_states {
	FPGAHP_MGR_UNKNOWN,
	FPGAHP_MGR_LOADING,
	FPGAHP_MGR_LOAD_DONE,
	FPGAHP_MGR_HP_FAIL,
};

/**
 * struct fpgahp_manager - represent a fpga hotplug manager instance
 *
 * @lock: mutex to protect fpgahp manager data
 * @is_registered: register status
 * @priv: private data for fpgahp manager
 * @ops: ops of this fpgahp_manager
 * @state: the status of fpgahp_manager
 * @name: name of the fpgahp_manager
 * @bmc: fpgahp BMC device
 */
struct fpgahp_manager {
	struct mutex lock; /* protect data structure contents */
	bool is_registered;
	void *priv;
	const struct fpgahp_manager_ops *ops;
	enum fpgahp_manager_states state;
	const char *name;
	struct fpgahp_bmc_device bmc;
};

static inline struct fpgahp_manager *to_fpgahp_mgr(struct fpgahp_bmc_device *bmc)
{
	return container_of(bmc, struct fpgahp_manager, bmc);
}

struct fpgahp_manager *fpgahp_register(struct pci_dev *hotplug_bridge,
				       const char *name, const struct fpgahp_manager_ops *ops,
				       void *priv);
void fpgahp_unregister(struct fpgahp_manager *mgr);

struct fpgahp_bmc_device *fpgahp_bmc_device_register(const struct fpgahp_bmc_ops *ops,
						     struct device *parent, void *priv);
void fpgahp_bmc_device_unregister(struct fpgahp_bmc_device *bmc);

#endif
