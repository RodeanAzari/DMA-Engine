/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Contract between the bus-independent core and a backend.
 *
 * A backend (host simulator today, PCI on Day 5) finds the device, fills
 * in a struct dmaeng_dev and calls dmaeng_core_add(). The core reaches
 * the device only through the ops table, never directly.
 */
#ifndef DMAENG_CORE_H
#define DMAENG_CORE_H

#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "dmaeng_regs.h"

#define DRV_NAME "dmaeng"

/* Descriptor as the device sees it (spec section 5.3), little-endian */
struct dmaeng_desc {
	__le64 src;		/* source DMA address */
	__le64 dst;		/* destination DMA address */
	__le32 len;		/* bytes */
	__le32 flags;		/* DMAENG_DESC_OWN | _IRQ | _LAST */
	__le32 status;		/* written by device before clearing OWN */
	__le32 _rsvd;		/* must be zero */
};

/* Fail the build if the struct drifts from spec/dmaeng.yaml */
static_assert(sizeof(struct dmaeng_desc) == DMAENG_DESC_SIZE);
static_assert(offsetof(struct dmaeng_desc, src) == DMAENG_DESC_OFF_SRC);
static_assert(offsetof(struct dmaeng_desc, dst) == DMAENG_DESC_OFF_DST);
static_assert(offsetof(struct dmaeng_desc, len) == DMAENG_DESC_OFF_LEN);
static_assert(offsetof(struct dmaeng_desc, flags) == DMAENG_DESC_OFF_FLAGS);
static_assert(offsetof(struct dmaeng_desc, status) == DMAENG_DESC_OFF_STATUS);
static_assert(offsetof(struct dmaeng_desc, _rsvd) == DMAENG_DESC_OFF_RSVD);

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
	struct dmaeng_desc *ring;		/* CPU view of the ring */
	dma_addr_t ring_dma;			/* device view, in RING_BASE */
	u32 ring_entries;			/* descriptors in the ring */
	u32 tail;				/* next descriptor to fill */
	void *buf;				/* CPU view of the data buffer */
	dma_addr_t buf_dma;			/* device view */
	size_t buf_bytes;
	struct completion done;			/* signalled by the ISR */
	struct mutex submit_lock;		/* one transfer in flight */
};

int dmaeng_core_add(struct dmaeng_dev *d);
void dmaeng_core_remove(struct dmaeng_dev *d);

#endif /* DMAENG_CORE_H */
