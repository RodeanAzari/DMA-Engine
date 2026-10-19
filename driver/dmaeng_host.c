// SPDX-License-Identifier: GPL-2.0
/*
 * dmaeng_host.c - host backend: the dmaeng registers modelled in software,
 * on a dummy platform device so the core can allocate DMA memory.
 *
 * Step 2: registers only. The engine thread that consumes descriptors
 * on a TAIL write comes in step 4.
 */
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

#define HOST_NREGS	(DMAENG_REG_MAX_XFER / 4 + 1)

struct dmaeng_host {
	struct dmaeng_dev d;
	struct platform_device *pdev;
	u32 regs[HOST_NREGS];		/* register file, indexed by off / 4 */
	irq_handler_t handler;		/* called by the engine in step 4 */
};

static struct dmaeng_host *host;

/* Power-on / CTRL.RESET state, per spec section 5.2 */
static void host_reset(struct dmaeng_host *h)
{
	memset(h->regs, 0, sizeof(h->regs));
	h->regs[DMAENG_REG_ID / 4] = DMAENG_ID_MAGIC;
	h->regs[DMAENG_REG_VERSION / 4] = DMAENG_VERSION;
	h->regs[DMAENG_REG_MAX_XFER / 4] = DMAENG_MAX_XFER_DEFAULT;
}

static u32 host_reg_read(struct dmaeng_dev *d, u32 off)
{
	struct dmaeng_host *h = d->priv;

	if (off % 4 || off / 4 >= HOST_NREGS)
		return 0;
	return h->regs[off / 4];
}

static void host_reg_write(struct dmaeng_dev *d, u32 off, u32 val)
{
	struct dmaeng_host *h = d->priv;

	if (off % 4 || off / 4 >= HOST_NREGS)
		return;

	switch (off) {
	case DMAENG_REG_ID:
	case DMAENG_REG_VERSION:
	case DMAENG_REG_STATUS:
	case DMAENG_REG_HEAD:
	case DMAENG_REG_ERR_CODE:
	case DMAENG_REG_MAX_XFER:
		break;				/* read-only */
	case DMAENG_REG_CTRL:
		if (val & DMAENG_CTRL_RESET)
			host_reset(h);		/* self-clearing */
		else
			h->regs[off / 4] = val;
		break;
	case DMAENG_REG_IRQ_STATUS:
		h->regs[off / 4] &= ~val;	/* write-1-to-clear */
		break;
	case DMAENG_REG_TAIL:
		h->regs[off / 4] = val;
		/* step 4: doorbell wakes the engine thread */
		break;
	default:
		h->regs[off / 4] = val;
		break;
	}
}

static int host_irq_setup(struct dmaeng_dev *d, irq_handler_t handler)
{
	struct dmaeng_host *h = d->priv;

	h->handler = handler;
	return 0;
}

static void host_irq_teardown(struct dmaeng_dev *d)
{
	struct dmaeng_host *h = d->priv;

	h->handler = NULL;
}

static const struct dmaeng_backend_ops host_ops = {
	.reg_read	= host_reg_read,
	.reg_write	= host_reg_write,
	.irq_setup	= host_irq_setup,
	.irq_teardown	= host_irq_teardown,
};

static int __init dmaeng_host_init(void)
{
	int ret;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->pdev = platform_device_register_simple("dmaeng-host",
						     PLATFORM_DEVID_NONE,
						     NULL, 0);
	if (IS_ERR(host->pdev)) {
		ret = PTR_ERR(host->pdev);
		goto err_free;
	}

	ret = dma_coerce_mask_and_coherent(&host->pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_unregister;

	host_reset(host);
	host->d.dev = &host->pdev->dev;
	host->d.ops = &host_ops;
	host->d.priv = host;

	ret = dmaeng_core_add(&host->d);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	platform_device_unregister(host->pdev);
err_free:
	kfree(host);
	return ret;
}

static void __exit dmaeng_host_exit(void)
{
	dmaeng_core_remove(&host->d);
	platform_device_unregister(host->pdev);
	kfree(host);
}

module_init(dmaeng_host_init);
module_exit(dmaeng_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dmaeng driver, host backend with simulated engine");
