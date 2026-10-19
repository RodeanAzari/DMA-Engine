/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dmaeng_core.h - contract between the bus-independent core and a backend.
 *
 * A backend (host simulator today, PCI on Day 5) finds the device, fills
 * in a struct dmaeng_dev and calls dmaeng_core_add(). The core reaches
 * the device only through the ops table, never directly.
 */
#ifndef DMAENG_CORE_H
#define DMAENG_CORE_H

#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/types.h>

#define DRV_NAME "dmaeng"

struct dmaeng_dev;

struct dmaeng_backend_ops {
	u32 (*reg_read)(struct dmaeng_dev *d, u32 off);
	void (*reg_write)(struct dmaeng_dev *d, u32 off, u32 val);
	int (*irq_setup)(struct dmaeng_dev *d, irq_handler_t handler);
	void (*irq_teardown)(struct dmaeng_dev *d);
};

struct dmaeng_dev {
	struct device *dev;			/* DMA-capable bus device */
	const struct dmaeng_backend_ops *ops;	/* how to reach the device */
	void *priv;				/* backend private state */
	struct miscdevice misc;			/* /dev/dmaeng, embedded */
};

int dmaeng_core_add(struct dmaeng_dev *d);
void dmaeng_core_remove(struct dmaeng_dev *d);

#endif /* DMAENG_CORE_H */
