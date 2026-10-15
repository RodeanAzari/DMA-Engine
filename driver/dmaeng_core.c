// SPDX-License-Identifier: GPL-2.0
/*
 * dmaeng_core.c - bus-independent core of the dmaeng driver.
 *
 * Step 1: only registers a misc char device so we can prove the
 * build/load/unload loop. Ring, engine, ioctl and mmap come later.
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#define DRV_NAME "dmaeng"

static int dmaeng_open(struct inode *inode, struct file *filp)
{
	pr_info(DRV_NAME ": open\n");
	return 0;
}

static int dmaeng_release(struct inode *inode, struct file *filp)
{
	pr_info(DRV_NAME ": release\n");
	return 0;
}

static const struct file_operations dmaeng_fops = {
	.owner		= THIS_MODULE,
	.open		= dmaeng_open,
	.release	= dmaeng_release,
};

static struct miscdevice dmaeng_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= DRV_NAME,
	.fops	= &dmaeng_fops,
};

static int __init dmaeng_init(void)
{
	int ret;

	ret = misc_register(&dmaeng_misc);
	if (ret) {
		pr_err(DRV_NAME ": misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info(DRV_NAME ": loaded\n");
	return 0;
}

static void __exit dmaeng_exit(void)
{
	misc_deregister(&dmaeng_misc);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(dmaeng_init);
module_exit(dmaeng_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MMIO DMA engine driver core");
