
#ifndef _LINUX_DFL_IMAGE_RELOAD_H
#define _LINUX_DFL_IMAGE_RELOAD_H

#include <linux/device.h>

struct dfl_image_reload;
struct dfl_image_trigger;

/**
 * struct dfl_image_reload_ops - image reload specific operations
 * @prepare: Required: Prepare image reload, remove non-reserved devices
 */
struct dfl_image_reload_ops {
	int (*prepare)(struct dfl_image_reload *dfl_reload);
};

/**
 * struct dfl_image_trigger_ops - image trigger specific operations
 * @available_images: Required: available images for reload trigger
 * @image_trigger: Required: trigger the image reload on BMC
 */
struct dfl_image_trigger_ops {
	ssize_t (*available_images)(struct dfl_image_trigger *trigger, char *buf);
	int (*image_trigger)(struct dfl_image_trigger *trigger, const char *buf);
};

struct dfl_image_trigger {
	const struct dfl_image_trigger_ops *ops;
	void *priv;
};

struct dfl_image_reload {
	struct device dev;
	const struct dfl_image_reload_ops *ops;
	struct mutex lock;    /* protect data structure contents */
	struct dfl_image_trigger trigger;
	void *priv;
};

/* Timeout (10s) for image reload */
#define RELOAD_TIMEOUT_MS  (10 * 1000)

struct dfl_image_reload *
dfl_image_reload_dev_register(const struct dfl_image_reload_ops *ops, void *priv);
void dfl_image_reload_dev_unregister(struct dfl_image_reload *dfl_reload);
struct dfl_image_trigger *
dfl_image_reload_trigger_register(const struct dfl_image_trigger_ops *ops, void *priv);
void dfl_image_reload_trigger_unregister(struct dfl_image_trigger *trigger);

#endif

