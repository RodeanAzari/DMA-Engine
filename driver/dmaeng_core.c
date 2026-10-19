// SPDX-License-Identifier: GPL-2.0
/*
 * Bus-independent core of the dmaeng driver.
 *
 * Talks to the device only through d->ops, so the same code runs on the
 * host simulator and on the PCI device. Ring, ioctl and mmap come later.
 */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

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

	id = d->ops->reg_read(d, DMAENG_REG_ID);
	if (id != DMAENG_ID_MAGIC) {
		dev_err(d->dev, "bad ID 0x%08x, expected 0x%08x\n",
			id, DMAENG_ID_MAGIC);
		return -ENODEV;
	}

	dev_info(d->dev, "version 0x%08x, max_xfer %u\n",
		 d->ops->reg_read(d, DMAENG_REG_VERSION),
		 d->ops->reg_read(d, DMAENG_REG_MAX_XFER));

	d->misc.minor = MISC_DYNAMIC_MINOR;
	d->misc.name = DRV_NAME;
	d->misc.fops = &dmaeng_fops;
	d->misc.parent = d->dev;

	ret = misc_register(&d->misc);
	if (ret) {
		dev_err(d->dev, "misc_register failed: %d\n", ret);
		return ret;
	}

	return 0;
}

void dmaeng_core_remove(struct dmaeng_dev *d)
{
	misc_deregister(&d->misc);
}
