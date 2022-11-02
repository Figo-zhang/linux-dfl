/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FPGA Card Framework
 *
 *  Copyright (C) 2022 Intel Corporation
 */
#ifndef _LINUX_FPGA_CARD_H
#define _LINUX_FPGA_CARD_H

#include <linux/mutex.h>
#include <linux/platform_device.h>

struct fpga_card;
struct sg_table;

/**
 * enum fpga_card_states - fpga card framework states
 * @FPGA_MGR_STATE_UNKNOWN: can't determine state
 * @FPGA_CARD_STATE_RELOAD_PREPARE: prepare for fpga card image reload
 * @FPGA_CARD_STATE_RELOAD_DONE: card reload done
 */
enum fpga_card_states {
	/* default FPGA states */
	FPGA_CARD_STATE_UNKNOWN,
	FPGA_CARD_STATE_RELOAD_PREPARE,
	FPGA_CARD_STATE_RELOAD_DONE
};

/**
 * struct fpga_card_info - collection of parameters for an FPGA Card
 * @name: fpga card name
 * @mops: pointer to structure of fpga card ops
 * @priv: fpga card private data
 *
 * fpga_card_info contains parameters for the register_full function.
 * These are separated into an info structure because they some are optional
 * others could be added to in the future. The info structure facilitates
 * maintaining a stable API.
 */
struct fpga_card_info {
	const char *name;
	const struct fpga_card_ops *mops;
	void *priv;
};

/**
 * struct fpga_card_ops - ops for low level fpga card drivers
 * @state: returns an enum value of the FPGA's state
 * @card_remove: optional: Set card into a specific state during driver remove
 * @reload_prepare: optional: prepare the FPGA before trigger the image reload
 * @groups: optional attribute groups.
 *
 * fpga_card_ops are the low level functions implemented by a specific
 * fpga card driver.  The optional ones are tested for NULL before being
 * called, so leaving them out is fine.
 */
struct fpga_card_ops {
	enum fpga_card_states (*state)(struct fpga_card *card);
	void (*card_remove)(struct fpga_card *card);
	int (*reload_prepare)(struct fpga_card *card);
	const struct attribute_group **groups;
};

/**
 * struct fpga_card - fpga card structure
 * @name: name of low level fpga card
 * @dev: fpga card device
 * @ref_mutex: only allows one reference to fpga card
 * @state: state of fpga card
 * @compat_id: FPGA card id for compatibility check.
 * @mops: pointer to struct of fpga card ops
 * @priv: low level driver private date
 */
struct fpga_card {
	const char *name;
	struct device dev;
	struct mutex ref_mutex; /* protect data structure contents */
	enum fpga_card_states state;
	struct fpga_compat_id *compat_id;
	const struct fpga_card_ops *mops;
	void *priv;
};

#define to_fpga_card(d) container_of(d, struct fpga_card, dev)

int fpga_card_prepare_image_reload(struct fpga_card *card);

int fpga_card_lock(struct fpga_card *card);
void fpga_card_unlock(struct fpga_card *card);

struct fpga_card *
fpga_card_register_full(struct device *parent, const struct fpga_card_info *info);

struct fpga_card *
fpga_card_register(struct device *parent, const char *name,
		   const struct fpga_card_ops *mops, void *priv);
void fpga_card_unregister(struct fpga_card *card);

struct fpga_card *
devm_fpga_card_register_full(struct device *parent, const struct fpga_card_info *info);
struct fpga_card *
devm_fpga_card_register(struct device *parent, const char *name,
			const struct fpga_card_ops *mops, void *priv);

#endif /*_LINUX_FPGA_CARD_H */
