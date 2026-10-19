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

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

/* Ring entries: power of two in [RING_SIZE_MIN, RING_SIZE_MAX] (spec 5.6) */
#define DMAENG_RING_ENTRIES	256
#define DMAENG_RING_BYTES	(DMAENG_RING_ENTRIES * sizeof(struct dmaeng_desc))

static_assert((DMAENG_RING_ENTRIES & (DMAENG_RING_ENTRIES - 1)) == 0);
static_assert(DMAENG_RING_ENTRIES >= DMAENG_RING_SIZE_MIN &&
	      DMAENG_RING_ENTRIES <= DMAENG_RING_SIZE_MAX);

static u32 dmaeng_rd(struct dmaeng_dev *d, u32 off)
{
	return d->ops->reg_read(d, off);
}

static void dmaeng_wr(struct dmaeng_dev *d, u32 off, u32 val)
{
	d->ops->reg_write(d, off, val);
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

	/* Spec 5.6 rule 3: _HI first, then _LO */
	dmaeng_wr(d, DMAENG_REG_RING_BASE_HI, upper_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_BASE_LO, lower_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_SIZE, DMAENG_RING_ENTRIES);
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_ENABLE);

	dev_info(d->dev, "ring %u x %zu B at dma %pad; device base 0x%08x%08x size %u ctrl 0x%x\n",
		 DMAENG_RING_ENTRIES, sizeof(struct dmaeng_desc), &d->ring_dma,
		 dmaeng_rd(d, DMAENG_REG_RING_BASE_HI),
		 dmaeng_rd(d, DMAENG_REG_RING_BASE_LO),
		 dmaeng_rd(d, DMAENG_REG_RING_SIZE),
		 dmaeng_rd(d, DMAENG_REG_CTRL));

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
	dma_free_coherent(d->dev, DMAENG_RING_BYTES, d->ring, d->ring_dma);
	return ret;
}

void dmaeng_core_remove(struct dmaeng_dev *d)
{
	misc_deregister(&d->misc);
	/* Stop the device before freeing memory it can DMA into */
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_RESET);
	dma_free_coherent(d->dev, DMAENG_RING_BYTES, d->ring, d->ring_dma);
	dev_info(d->dev, "removed\n");
}
