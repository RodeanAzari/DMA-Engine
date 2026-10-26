// SPDX-License-Identifier: GPL-2.0
/*
 * Host backend: the dmaeng registers modelled in software,
 * on a dummy platform device so the core can allocate DMA memory.
 *
 * A TAIL write wakes a kernel thread that plays the engine: it walks the
 * ring from HEAD exactly as the QEMU model and the RTL will. Completion
 * interrupts are delivered through a hard irq_work, so the core's ISR
 * runs in hardirq context just as it does with MSI.
 */
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

#define HOST_NREGS	(DMAENG_REG_MAX_XFER / 4 + 1)
#define REG(off)	((off) / 4)

struct dmaeng_host {
	struct dmaeng_dev d;
	struct platform_device *pdev;
	spinlock_t lock;		/* regs, irq_level, engine progress */
	u32 regs[HOST_NREGS];		/* register file, indexed by off / 4 */
	bool irq_level;			/* IRQ_STATUS & IRQ_MASK != 0 */
	irq_handler_t handler;		/* the core's ISR */
	struct irq_work irq_work;	/* delivers the interrupt */
	struct task_struct *engine;
	wait_queue_head_t wq;		/* engine sleeps here */
};

static struct dmaeng_host *host;

/* Power-on / CTRL.RESET state, per spec section 5.2. Caller holds lock. */
static void host_reset(struct dmaeng_host *h)
{
	memset(h->regs, 0, sizeof(h->regs));
	h->regs[REG(DMAENG_REG_ID)] = DMAENG_ID_MAGIC;
	h->regs[REG(DMAENG_REG_VERSION)] = DMAENG_VERSION;
	h->regs[REG(DMAENG_REG_MAX_XFER)] = DMAENG_MAX_XFER_DEFAULT;
	h->irq_level = false;
}

/* Spec 5.4: the interrupt is the level IRQ_STATUS & IRQ_MASK; fire on 0->1 */
static void host_update_irq(struct dmaeng_host *h)
{
	bool level = h->regs[REG(DMAENG_REG_IRQ_STATUS)] &
		     h->regs[REG(DMAENG_REG_IRQ_MASK)];

	if (level && !h->irq_level)
		irq_work_queue(&h->irq_work);
	h->irq_level = level;
}

static void host_irq_work(struct irq_work *work)
{
	struct dmaeng_host *h = container_of(work, struct dmaeng_host,
					     irq_work);
	irq_handler_t handler = READ_ONCE(h->handler);

	if (handler)
		handler(0, &h->d);
}

/* Caller holds lock */
static bool host_has_work(struct dmaeng_host *h)
{
	return (h->regs[REG(DMAENG_REG_CTRL)] & DMAENG_CTRL_ENABLE) &&
	       !(h->regs[REG(DMAENG_REG_STATUS)] & DMAENG_STATUS_ERROR) &&
	       h->regs[REG(DMAENG_REG_HEAD)] != h->regs[REG(DMAENG_REG_TAIL)];
}

/* Caller holds lock */
static void host_update_busy(struct dmaeng_host *h)
{
	if (host_has_work(h))
		h->regs[REG(DMAENG_REG_STATUS)] |= DMAENG_STATUS_BUSY;
	else
		h->regs[REG(DMAENG_REG_STATUS)] &= ~DMAENG_STATUS_BUSY;
}

/* Spec 5.5: record the error and stop until CTRL.RESET. Caller holds lock. */
static void host_error(struct dmaeng_host *h, u32 code)
{
	h->regs[REG(DMAENG_REG_ERR_CODE)] = code;
	h->regs[REG(DMAENG_REG_STATUS)] |= DMAENG_STATUS_ERROR;
	h->regs[REG(DMAENG_REG_IRQ_STATUS)] |= DMAENG_IRQ_STATUS_ERROR;
	host_update_irq(h);
}

static bool in_window(u64 addr, u32 len, dma_addr_t base, size_t size)
{
	return addr >= base && len <= size && addr - base <= size - len;
}

/*
 * The simulated engine reaches only memory the core allocated: the ring
 * and the data buffer. Any other address is error 0x2, like an IOMMU
 * fault. Real hardware would DMA anywhere; here a bad address would
 * corrupt host kernel memory.
 */
static struct dmaeng_desc *host_desc(struct dmaeng_host *h, u64 addr)
{
	struct dmaeng_dev *d = &h->d;
	size_t ring_bytes = d->ring_entries * sizeof(struct dmaeng_desc);

	if (!in_window(addr, sizeof(struct dmaeng_desc), d->ring_dma,
		       ring_bytes))
		return NULL;
	return (void *)d->ring + (addr - d->ring_dma);
}

static void *host_data(struct dmaeng_host *h, u64 addr, u32 len)
{
	struct dmaeng_dev *d = &h->d;

	if (!in_window(addr, len, d->buf_dma, d->buf_bytes))
		return NULL;
	return d->buf + (addr - d->buf_dma);
}

/* Process the descriptor at HEAD (spec 5.4). Caller holds lock. */
static void host_step(struct dmaeng_host *h)
{
	u32 *r = h->regs;
	u32 head = r[REG(DMAENG_REG_HEAD)];
	u32 size = r[REG(DMAENG_REG_RING_SIZE)];
	u64 base = (u64)r[REG(DMAENG_REG_RING_BASE_HI)] << 32 |
		   r[REG(DMAENG_REG_RING_BASE_LO)];
	struct dmaeng_desc *desc;
	void *src, *dst;
	u32 flags, len, err;

	if (base % DMAENG_RING_ALIGN || !is_power_of_2(size) ||
	    size < DMAENG_RING_SIZE_MIN || size > DMAENG_RING_SIZE_MAX) {
		host_error(h, DMAENG_ERR_BAD_DESC);
		return;
	}

	desc = host_desc(h, base + (u64)head * sizeof(*desc));
	if (!desc) {
		host_error(h, DMAENG_ERR_DMA_FAIL);
		return;
	}

	flags = le32_to_cpu(READ_ONCE(desc->flags));
	if (!(flags & DMAENG_DESC_OWN)) {
		/* HEAD != TAIL but not ours: driver bug, nothing to write */
		host_error(h, DMAENG_ERR_BAD_DESC);
		return;
	}
	dma_rmb();			/* OWN seen before the other fields */

	len = le32_to_cpu(desc->len);
	src = host_data(h, le64_to_cpu(desc->src), len);
	dst = host_data(h, le64_to_cpu(desc->dst), len);

	if (desc->_rsvd)
		err = DMAENG_ERR_BAD_DESC;
	else if (!len || len > r[REG(DMAENG_REG_MAX_XFER)])
		err = DMAENG_ERR_BAD_LEN;
	else if (!src || !dst)
		err = DMAENG_ERR_DMA_FAIL;
	else
		err = DMAENG_ERR_OK;

	if (!err)
		memcpy(dst, src, len);

	/* Spec 5.6 rule 5: status visible before OWN clears */
	WRITE_ONCE(desc->status, cpu_to_le32(err));
	dma_wmb();
	WRITE_ONCE(desc->flags, cpu_to_le32(flags & ~DMAENG_DESC_OWN));

	r[REG(DMAENG_REG_HEAD)] = (head + 1) & (size - 1);

	if (err) {
		host_error(h, err);
		return;
	}
	if (flags & DMAENG_DESC_IRQ) {
		r[REG(DMAENG_REG_IRQ_STATUS)] |= DMAENG_IRQ_STATUS_COMPLETION;
		host_update_irq(h);
	}
}

static bool host_pending(struct dmaeng_host *h)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&h->lock, flags);
	ret = host_has_work(h);
	spin_unlock_irqrestore(&h->lock, flags);
	return ret;
}

static int host_engine(void *arg)
{
	struct dmaeng_host *h = arg;
	unsigned long flags;

	while (!kthread_should_stop()) {
		wait_event_interruptible(h->wq, host_pending(h) ||
					 kthread_should_stop());

		/* One descriptor per lock hold, so a reset waits for it */
		spin_lock_irqsave(&h->lock, flags);
		if (host_has_work(h))
			host_step(h);
		host_update_busy(h);
		spin_unlock_irqrestore(&h->lock, flags);

		cond_resched();
	}
	return 0;
}

static u32 host_reg_read(struct dmaeng_dev *d, u32 off)
{
	struct dmaeng_host *h = d->priv;
	unsigned long flags;
	u32 val;

	if (off % 4 || REG(off) >= HOST_NREGS)
		return 0;

	spin_lock_irqsave(&h->lock, flags);
	val = h->regs[REG(off)];
	spin_unlock_irqrestore(&h->lock, flags);
	return val;
}

static void host_reg_write(struct dmaeng_dev *d, u32 off, u32 val)
{
	struct dmaeng_host *h = d->priv;
	unsigned long flags;

	if (off % 4 || REG(off) >= HOST_NREGS)
		return;

	spin_lock_irqsave(&h->lock, flags);
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
			h->regs[REG(off)] = val;
		host_update_busy(h);
		wake_up(&h->wq);
		break;
	case DMAENG_REG_IRQ_STATUS:
		h->regs[REG(off)] &= ~val;	/* write-1-to-clear */
		host_update_irq(h);
		break;
	case DMAENG_REG_IRQ_MASK:
		h->regs[REG(off)] = val;
		host_update_irq(h);
		break;
	case DMAENG_REG_TAIL:
		h->regs[REG(off)] = val;	/* doorbell */
		host_update_busy(h);
		wake_up(&h->wq);
		break;
	default:
		h->regs[REG(off)] = val;
		break;
	}
	spin_unlock_irqrestore(&h->lock, flags);
}

static int host_irq_setup(struct dmaeng_dev *d, irq_handler_t handler)
{
	struct dmaeng_host *h = d->priv;

	WRITE_ONCE(h->handler, handler);
	return 0;
}

static void host_irq_teardown(struct dmaeng_dev *d)
{
	struct dmaeng_host *h = d->priv;

	WRITE_ONCE(h->handler, NULL);
	irq_work_sync(&h->irq_work);	/* no ISR still running */
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

	spin_lock_init(&host->lock);
	init_waitqueue_head(&host->wq);
	host->irq_work = IRQ_WORK_INIT_HARD(host_irq_work);
	host_reset(host);

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

	host->d.dev = &host->pdev->dev;
	host->d.ops = &host_ops;
	host->d.priv = host;

	host->engine = kthread_run(host_engine, host, "dmaeng-engine");
	if (IS_ERR(host->engine)) {
		ret = PTR_ERR(host->engine);
		goto err_unregister;
	}

	ret = dmaeng_core_add(&host->d);
	if (ret)
		goto err_stop;

	return 0;

err_stop:
	kthread_stop(host->engine);
err_unregister:
	platform_device_unregister(host->pdev);
err_free:
	kfree(host);
	return ret;
}

static void __exit dmaeng_host_exit(void)
{
	dmaeng_core_remove(&host->d);
	kthread_stop(host->engine);
	platform_device_unregister(host->pdev);
	kfree(host);
}

module_init(dmaeng_host_init);
module_exit(dmaeng_host_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dmaeng driver, host backend with simulated engine");
