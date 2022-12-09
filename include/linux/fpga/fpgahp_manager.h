/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver Header File for FPGA PCI Hotplug Driver
 *
 * Copyright (C) 2023 Intel Corporation
 */
#ifndef _LINUX_FPGAHP_MANAGER_H
#define _LINUX_FPGAHP_MANAGER_H

#include <linux/mutex.h>

struct pci_dev;
struct fpgahp_manager;

/**
 * struct fpgahp_manager_ops - fpgahp manager specific operations
 * @hotplug_prepare: Required: hotplug prepare like removing subdevices
 *                   below the PCI device.
 */
struct fpgahp_manager_ops {
	int (*hotplug_prepare)(struct fpgahp_manager *mgr);
};

/**
 * enum fpgahp_manager_states - FPGA hotplug states
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
 * struct fpgahp_manager - represent a FPGA hotplug manager instance
 *
 * @lock: mutex to protect fpgahp manager data
 * @priv: private data for fpgahp manager
 * @ops: ops of this fpgahp_manager
 * @state: the status of fpgahp_manager
 * @name: name of the fpgahp_manager
 * @registered: register status
 */
struct fpgahp_manager {
	struct mutex lock; /* protect registered state of fpgahp_manager */
	void *priv;
	const struct fpgahp_manager_ops *ops;
	enum fpgahp_manager_states state;
	const char *name;
	bool registered;
};

struct fpgahp_manager *fpgahp_register(struct pci_dev *hotplug_bridge,
				       const char *name, const struct fpgahp_manager_ops *ops,
				       void *priv);
void fpgahp_unregister(struct fpgahp_manager *mgr);

#endif
