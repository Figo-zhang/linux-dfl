
#ifndef _LINUX_DFL_FPGA_RELOAD_H
#define _LINUX_DFL_FPGA_RELOAD_H

#include <linux/device.h>

struct dfl_fpga_reload;

struct dfl_fpga_reload_ops {
	int (*prepare)(struct dfl_fpga_reload *dfl_reload);
	int (*remove)(struct dfl_fpga_reload *dfl_reload);
};

struct dfl_fpga_reload {
	struct device dev;
	struct module *module;
	const struct dfl_fpga_reload_ops *ops;
	struct mutex lock;    /* protect data structure contents */
	void *priv;
};

struct dfl_fpga_reload *
dfl_fpga_reload_dev_register(struct module *module,
                const struct dfl_fpga_reload_ops *ops, void *priv);
void dfl_fpga_reload_dev_unregister(struct dfl_fpga_reload *dfl_reload);

#endif

