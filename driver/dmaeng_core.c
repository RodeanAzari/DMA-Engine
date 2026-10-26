// SPDX-License-Identifier: GPL-2.0
/*
 * Bus-independent core of the dmaeng driver.
 *
 * Talks to the device only through d->ops, so the same code runs on the
 * host simulator and on the PCI device. Ioctl and mmap come later.
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

/* Ring entries: power of two in [RING_SIZE_MIN, RING_SIZE_MAX] (spec 5.6) */
#define DMAENG_RING_ENTRIES	256
#define DMAENG_RING_BYTES	(DMAENG_RING_ENTRIES * sizeof(struct dmaeng_desc))

/* Data buffer: source and destination of every transfer, mmap'd later */
#define DMAENG_BUF_BYTES	SZ_1M

static_assert((DMAENG_RING_ENTRIES & (DMAENG_RING_ENTRIES - 1)) == 0);
static_assert((DMAENG_RING_ENTRIES >= DMAENG_RING_SIZE_MIN) &&
	      (DMAENG_RING_ENTRIES <= DMAENG_RING_SIZE_MAX));

static u32 dmaeng_rd(struct dmaeng_dev *d, u32 off)
{
	return d->ops->reg_read(d, off);
}

static void dmaeng_wr(struct dmaeng_dev *d, u32 off, u32 val)
{
	d->ops->reg_write(d, off, val);
}

static irqreturn_t dmaeng_isr(int irq, void *dev_id)
{
	struct dmaeng_dev *d = dev_id;
	u32 st;

	st = dmaeng_rd(d, DMAENG_REG_IRQ_STATUS);
	if (!st)
		return IRQ_NONE;

	/* Clear before the waiter scans the ring, or a new event is lost */
	dmaeng_wr(d, DMAENG_REG_IRQ_STATUS, st);
	complete(&d->done);
	return IRQ_HANDLED;
}

/*
 * Submit one copy within the data buffer and wait for it (queue depth 1).
 * Returns the descriptor status (0 = success, spec 5.5) or -errno.
 */
static int dmaeng_xfer(struct dmaeng_dev *d, u32 src_off, u32 dst_off, u32 len)
{
	struct dmaeng_desc *desc = &d->ring[d->tail];
	u32 flags;

	reinit_completion(&d->done);

	desc->src = cpu_to_le64(d->buf_dma + src_off);
	desc->dst = cpu_to_le64(d->buf_dma + dst_off);
	desc->len = cpu_to_le32(len);
	desc->status = 0;
	desc->_rsvd = 0;
	desc->flags = cpu_to_le32(DMAENG_DESC_OWN | DMAENG_DESC_IRQ |
				  DMAENG_DESC_LAST);
	d->tail = (d->tail + 1) & (d->ring_entries - 1);

	wmb();				/* descriptor visible before doorbell */
	dmaeng_wr(d, DMAENG_REG_TAIL, d->tail);

	if (!wait_for_completion_timeout(&d->done, HZ))
		return -ETIMEDOUT;

	flags = le32_to_cpu(READ_ONCE(desc->flags));
	if (flags & DMAENG_DESC_OWN)
		return -EIO;		/* interrupt without completion */
	dma_rmb();			/* OWN clear before reading status */
	return le32_to_cpu(READ_ONCE(desc->status));
}

/*
 * Temporary load-time check until the ioctl exists (step 5). More copies
 * than ring entries, so the indices wrap; lengths vary from 1 B to
 * MAX_XFER; a guard byte checks nothing is written past the end.
 */
static int dmaeng_selftest(struct dmaeng_dev *d)
{
	u8 *src = d->buf;
	u8 *dst = d->buf + SZ_128K;
	u32 i, len;
	int ret;

	for (i = 0; i < d->ring_entries + 8; i++) {
		len = 1 + (i * 4099) % DMAENG_MAX_XFER_DEFAULT;
		memset(src, i, len);
		memset(dst, ~i, len + 1);

		ret = dmaeng_xfer(d, 0, SZ_128K, len);
		if (ret) {
			dev_err(d->dev, "selftest %u: len %u status %d\n",
				i, len, ret);
			return ret < 0 ? ret : -EIO;
		}
		if (memcmp(src, dst, len) || dst[len] != (u8)~i) {
			dev_err(d->dev, "selftest %u: len %u data mismatch\n",
				i, len);
			return -EIO;
		}
	}

	dev_info(d->dev, "selftest: %u copies OK, head %u tail %u\n",
		 i, dmaeng_rd(d, DMAENG_REG_HEAD), d->tail);
	return 0;
}

static int dmaeng_open(struct inode *inode, struct file *filp)
{
	/* misc_open() left our embedded miscdevice in private_data */
	struct dmaeng_dev *d = container_of(filp->private_data,
					    struct dmaeng_dev, misc);

	filp->private_data = d;
	dev_info(d->dev, "open\n");
	return 0;
}

static int dmaeng_release(struct inode *inode, struct file *filp)
{
	struct dmaeng_dev *d = filp->private_data;

	dev_info(d->dev, "release\n");
	return 0;
}

static const struct file_operations dmaeng_fops = {
	.owner		= THIS_MODULE,
	.open		= dmaeng_open,
	.release	= dmaeng_release,
};

int dmaeng_core_add(struct dmaeng_dev *d)
{
	u32 id;
	int ret;

	id = dmaeng_rd(d, DMAENG_REG_ID);
	if (id != DMAENG_ID_MAGIC) {
		dev_err(d->dev, "bad ID 0x%08x, expected 0x%08x\n",
			id, DMAENG_ID_MAGIC);
		return -ENODEV;
	}

	dev_info(d->dev, "version 0x%08x, max_xfer %u\n",
		 dmaeng_rd(d, DMAENG_REG_VERSION),
		 dmaeng_rd(d, DMAENG_REG_MAX_XFER));

	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_RESET);

	/* Zeroed and page-aligned, so every OWN bit starts clear */
	d->ring = dma_alloc_coherent(d->dev, DMAENG_RING_BYTES,
				     &d->ring_dma, GFP_KERNEL);
	if (!d->ring)
		return -ENOMEM;
	d->ring_entries = DMAENG_RING_ENTRIES;
	d->tail = 0;

	d->buf = dma_alloc_coherent(d->dev, DMAENG_BUF_BYTES,
				    &d->buf_dma, GFP_KERNEL);
	if (!d->buf) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	d->buf_bytes = DMAENG_BUF_BYTES;

	init_completion(&d->done);
	ret = d->ops->irq_setup(d, dmaeng_isr);
	if (ret) {
		dev_err(d->dev, "irq_setup failed: %d\n", ret);
		goto err_free_buf;
	}

	/* Spec 5.6 rule 3: _HI first, then _LO */
	dmaeng_wr(d, DMAENG_REG_RING_BASE_HI, upper_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_BASE_LO, lower_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_SIZE, DMAENG_RING_ENTRIES);
	dmaeng_wr(d, DMAENG_REG_IRQ_MASK,
		  DMAENG_IRQ_MASK_COMPLETION | DMAENG_IRQ_MASK_ERROR);
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_ENABLE);

	dev_info(d->dev, "ring %u x %zu B at dma %pad; device base 0x%08x%08x size %u ctrl 0x%x\n",
		 DMAENG_RING_ENTRIES, sizeof(struct dmaeng_desc), &d->ring_dma,
		 dmaeng_rd(d, DMAENG_REG_RING_BASE_HI),
		 dmaeng_rd(d, DMAENG_REG_RING_BASE_LO),
		 dmaeng_rd(d, DMAENG_REG_RING_SIZE),
		 dmaeng_rd(d, DMAENG_REG_CTRL));

	ret = dmaeng_selftest(d);
	if (ret)
		goto err_disable;

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = DRV_NAME;
	d->misc.fops = &dmaeng_fops;
	d->misc.parent = d->dev;

	ret = misc_register(&d->misc);
	if (ret) {
		dev_err(d->dev, "misc_register failed: %d\n", ret);
		goto err_disable;
	}

	return 0;

err_disable:
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_RESET);
	d->ops->irq_teardown(d);
err_free_buf:
	dma_free_coherent(d->dev, DMAENG_BUF_BYTES, d->buf, d->buf_dma);
err_free_ring:
	dma_free_coherent(d->dev, DMAENG_RING_BYTES, d->ring, d->ring_dma);
	return ret;
}

void dmaeng_core_remove(struct dmaeng_dev *d)
{
	misc_deregister(&d->misc);
	/*
	 * Stop the device before freeing memory it can DMA into, and
	 * silence it before removing the interrupt handler.
	 */
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_RESET);
	d->ops->irq_teardown(d);
	dma_free_coherent(d->dev, DMAENG_BUF_BYTES, d->buf, d->buf_dma);
	dma_free_coherent(d->dev, DMAENG_RING_BYTES, d->ring, d->ring_dma);
	dev_info(d->dev, "removed\n");
}
