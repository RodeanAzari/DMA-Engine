// SPDX-License-Identifier: GPL-2.0
/*
 * Bus-independent core of the dmaeng driver: ring, char device, ioctl, mmap.
 *
 * Talks to the device only through d->ops, so the same code runs on the
 * host simulator and on the PCI device.
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"
#include "dmaeng_uapi.h"

/* Ring entries: power of two in [RING_SIZE_MIN, RING_SIZE_MAX] (spec 5.6) */
#define DMAENG_RING_ENTRIES	256
#define DMAENG_RING_BYTES	(DMAENG_RING_ENTRIES * sizeof(struct dmaeng_desc))

/* Data buffer: source and destination of every transfer, mmap'd to userspace */
#define DMAENG_BUF_BYTES	SZ_1M

#define DMAENG_XFER_TIMEOUT	HZ

#define DMAENG_XFER_PATH_FLAGS	(DMAENG_XFER_POLL | DMAENG_XFER_NOWAIT)

#ifdef DMAENG_FAULT_INJECT
#define DMAENG_XFER_VALID_FLAGS	(DMAENG_XFER_PATH_FLAGS | \
				 DMAENG_XFER_FAULT_BAD_ADDR | \
				 DMAENG_XFER_FAULT_RSVD | \
				 DMAENG_XFER_FAULT_NO_IRQ | \
				 DMAENG_XFER_FAULT_NO_OWN | \
				 DMAENG_XFER_FAULT_RING_ALIGN | \
				 DMAENG_XFER_FAULT_RING_ADDR)
#else
#define DMAENG_XFER_VALID_FLAGS	DMAENG_XFER_PATH_FLAGS
#endif

#define DMAENG_FAULT_ADDR	0xFFFFFFFFFFFF0000ULL

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
	if (st & DMAENG_IRQ_STATUS_ERROR)
		WRITE_ONCE(d->hw_error, true);
	complete(&d->done);
	return IRQ_HANDLED;
}

/*
 * Reset the device and start it on an empty ring. Reset clears every
 * register (spec 5.5), so this is also how the driver recovers from an
 * error or an abandoned transfer.
 */
static void dmaeng_hw_init(struct dmaeng_dev *d)
{
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_RESET);

	memset(d->ring, 0, DMAENG_RING_BYTES);
	d->tail = 0;
	d->nowait_desc = NULL;

	/* Spec 5.6 rule 3: _HI first, then _LO */
	dmaeng_wr(d, DMAENG_REG_RING_BASE_HI, upper_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_BASE_LO, lower_32_bits(d->ring_dma));
	dmaeng_wr(d, DMAENG_REG_RING_SIZE, DMAENG_RING_ENTRIES);
	dmaeng_wr(d, DMAENG_REG_IRQ_MASK,
		  DMAENG_IRQ_MASK_COMPLETION | DMAENG_IRQ_MASK_ERROR);
	dmaeng_wr(d, DMAENG_REG_CTRL, DMAENG_CTRL_ENABLE);
}

/*
 * Wait until the device gives desc back. An interrupt proves nothing by
 * itself (spec 5.6 rule 6: it can be late or spurious), so OWN decides.
 */
static int dmaeng_wait(struct dmaeng_dev *d, struct dmaeng_desc *desc)
{
	long left = DMAENG_XFER_TIMEOUT;

	for (;;) {
		left = wait_for_completion_interruptible_timeout(&d->done, left);
		if (left < 0)
			return -EINTR;	/* never restart: already submitted */
		if (!left)
			return -ETIMEDOUT;

		if (!(le32_to_cpu(READ_ONCE(desc->flags)) & DMAENG_DESC_OWN))
			return 0;
		if (dmaeng_rd(d, DMAENG_REG_STATUS) & DMAENG_STATUS_ERROR)
			return -EIO;	/* stopped before taking desc */
	}
}

/* Spin on OWN. No interrupt was requested, so this is the only way to know. */
static int dmaeng_poll(struct dmaeng_desc *desc)
{
	unsigned long deadline = jiffies + DMAENG_XFER_TIMEOUT;

	while (le32_to_cpu(READ_ONCE(desc->flags)) & DMAENG_DESC_OWN) {
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		cpu_relax();
		cond_resched();		/* the engine may need this CPU */
	}
	return 0;
}

/*
 * Queue depth stays 1 across a NOWAIT submit: the next submit first settles
 * the previous one. Nothing here reads a register unless the transfer has
 * not finished, so the common case adds no MMIO to the measured path.
 */
static int dmaeng_reap_nowait(struct dmaeng_dev *d)
{
	struct dmaeng_desc *desc = d->nowait_desc;

	if (!desc)
		return 0;

	if (le32_to_cpu(READ_ONCE(desc->flags)) & DMAENG_DESC_OWN) {
		if (!(dmaeng_rd(d, DMAENG_REG_STATUS) & DMAENG_STATUS_ERROR) &&
		    !time_after(jiffies, d->nowait_deadline))
			return -EBUSY;
		dmaeng_hw_init(d);
		return 0;
	}

	dma_rmb();			/* OWN clear before reading status */
	if (READ_ONCE(desc->status) || READ_ONCE(d->hw_error))
		dmaeng_hw_init(d);	/* the device stopped; restart it */
	d->nowait_desc = NULL;
	return 0;
}

static int dmaeng_status_to_errno(u32 status)
{
	switch (status) {
	case DMAENG_ERR_OK:
		return 0;
	case DMAENG_ERR_BAD_LEN:
		return -EINVAL;
	case DMAENG_ERR_DMA_FAIL:
		return -EFAULT;
	default:
		return -EIO;
	}
}

/*
 * Submit one copy within the data buffer and wait for it (queue depth 1).
 * Caller holds submit_lock.
 */
static int dmaeng_xfer(struct dmaeng_dev *d, const struct dmaeng_xfer *x,
		       u64 *lat_ns, u32 *desc_idx)
{
	struct dmaeng_desc *desc;
	u32 desc_flags = DMAENG_DESC_OWN | DMAENG_DESC_IRQ | DMAENG_DESC_LAST;
	u64 start;
	u32 status;
	int ret;

	ret = dmaeng_reap_nowait(d);
	if (ret)
		return ret;

	desc = &d->ring[d->tail];
	*desc_idx = d->tail;
	WRITE_ONCE(d->hw_error, false);
	reinit_completion(&d->done);

	desc->src = cpu_to_le64(d->buf_dma + x->src_off);
	desc->dst = cpu_to_le64(d->buf_dma + x->dst_off);
	desc->len = cpu_to_le32(x->len);
	desc->status = 0;
	desc->_rsvd = 0;

	/* x->flags can only be non-zero in a FAULT_INJECT build */
	if (x->flags & DMAENG_XFER_FAULT_BAD_ADDR)
		desc->src = cpu_to_le64(DMAENG_FAULT_ADDR);
	if (x->flags & DMAENG_XFER_FAULT_RSVD)
		desc->_rsvd = cpu_to_le32(1);
	if (x->flags & (DMAENG_XFER_FAULT_NO_IRQ | DMAENG_XFER_PATH_FLAGS))
		desc_flags &= ~DMAENG_DESC_IRQ;
	if (x->flags & DMAENG_XFER_FAULT_NO_OWN)
		desc_flags &= ~DMAENG_DESC_OWN;
	if (x->flags & DMAENG_XFER_FAULT_RING_ALIGN)
		dmaeng_wr(d, DMAENG_REG_RING_BASE_LO,
			  lower_32_bits(d->ring_dma) + 1);
	if (x->flags & DMAENG_XFER_FAULT_RING_ADDR) {
		dmaeng_wr(d, DMAENG_REG_RING_BASE_HI,
			  upper_32_bits(DMAENG_FAULT_ADDR));
		dmaeng_wr(d, DMAENG_REG_RING_BASE_LO,
			  lower_32_bits(DMAENG_FAULT_ADDR));
	}

	dma_wmb();			/* fields visible before OWN */
	desc->flags = cpu_to_le32(desc_flags);
	d->tail = (d->tail + 1) & (d->ring_entries - 1);

	start = ktime_get_ns();
	wmb();				/* descriptor visible before doorbell */
	dmaeng_wr(d, DMAENG_REG_TAIL, d->tail);

	if (x->flags & DMAENG_XFER_NOWAIT) {
		d->nowait_desc = desc;
		d->nowait_deadline = jiffies + DMAENG_XFER_TIMEOUT;
		*lat_ns = 0;
		return 0;
	}

	if (x->flags & DMAENG_XFER_POLL)
		ret = dmaeng_poll(desc);
	else
		ret = dmaeng_wait(d, desc);
	*lat_ns = ktime_get_ns() - start;
	if (!ret) {
		dma_rmb();		/* OWN clear before reading status */
		status = le32_to_cpu(READ_ONCE(desc->status));
		ret = dmaeng_status_to_errno(status);
		/* OWN clear and status 0 still fails if the device stopped */
		if (!ret && READ_ONCE(d->hw_error))
			ret = -EIO;
	}

	/* The device stops on an error, and may still own desc if we gave up */
	if (ret) {
		dev_err_ratelimited(d->dev, "transfer failed (%d), err_code 0x%x; resetting\n",
				    ret, dmaeng_rd(d, DMAENG_REG_ERR_CODE));
		dmaeng_hw_init(d);
	}
	return ret;
}

/* Written to survive any u64 offset without overflowing */
static bool dmaeng_in_buf(struct dmaeng_dev *d, u64 off, u32 len)
{
	return len <= d->buf_bytes && off <= d->buf_bytes - len;
}

static long dmaeng_ioctl_submit(struct dmaeng_dev *d, void __user *arg)
{
	struct dmaeng_xfer x;
	int ret;

	if (copy_from_user(&x, arg, sizeof(x)))
		return -EFAULT;

	/*
	 * Userspace must not be able to steer the engine outside the buffer.
	 * Length limits stay the device's job (spec 5.5), so only overlap,
	 * which has no defined copy order, is also refused here.
	 */
	if ((x.flags & ~DMAENG_XFER_VALID_FLAGS) || x._pad ||
	    (x.flags & DMAENG_XFER_PATH_FLAGS) == DMAENG_XFER_PATH_FLAGS ||
	    !dmaeng_in_buf(d, x.src_off, x.len) ||
	    !dmaeng_in_buf(d, x.dst_off, x.len))
		return -EINVAL;
	if (x.src_off < x.dst_off + x.len && x.dst_off < x.src_off + x.len)
		return -EINVAL;

	ret = mutex_lock_interruptible(&d->submit_lock);
	if (ret)
		return ret;
	ret = dmaeng_xfer(d, &x, &x.lat_ns, &x.desc_idx);
	mutex_unlock(&d->submit_lock);
	if (ret)
		return ret;

	return copy_to_user(arg, &x, sizeof(x)) ? -EFAULT : 0;
}

static long dmaeng_ioctl_get_info(struct dmaeng_dev *d, void __user *arg)
{
	struct dmaeng_info info = {
		.buf_bytes = d->buf_bytes,
		.ring_entries = d->ring_entries,
		.max_xfer = dmaeng_rd(d, DMAENG_REG_MAX_XFER),
	};

	return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long dmaeng_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dmaeng_dev *d = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case DMAENG_SUBMIT:
		return dmaeng_ioctl_submit(d, uarg);
	case DMAENG_GET_INFO:
		return dmaeng_ioctl_get_info(d, uarg);
	default:
		return -ENOTTY;
	}
}

static int dmaeng_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct dmaeng_dev *d = filp->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long pgoff = vma->vm_pgoff;

	/* A private mapping would copy on write and stop seeing the device */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	/* An unchecked length here would disclose kernel memory */
	switch (pgoff) {
	case DMAENG_MMAP_PGOFF_BUF:
		if (size > d->buf_bytes)
			return -EINVAL;
		break;
	case DMAENG_MMAP_PGOFF_RING:
		if (size > PAGE_ALIGN(DMAENG_RING_BYTES))
			return -EINVAL;
		/* Writing OWN or addresses here would steer the engine */
		if (vma->vm_flags & VM_WRITE)
			return -EPERM;
		vm_flags_clear(vma, VM_MAYWRITE);
		break;
	default:
		return -EINVAL;
	}

	/* The offset chose the region; it is not an offset into it */
	vma->vm_pgoff = 0;
	if (pgoff == DMAENG_MMAP_PGOFF_RING)
		return dma_mmap_coherent(d->dev, vma, d->ring, d->ring_dma,
					 DMAENG_RING_BYTES);
	return dma_mmap_coherent(d->dev, vma, d->buf, d->buf_dma, d->buf_bytes);
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
	.unlocked_ioctl	= dmaeng_ioctl,
	.mmap		= dmaeng_mmap,
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

	d->buf = dma_alloc_coherent(d->dev, DMAENG_BUF_BYTES,
				    &d->buf_dma, GFP_KERNEL);
	if (!d->buf) {
		ret = -ENOMEM;
		goto err_free_ring;
	}
	d->buf_bytes = DMAENG_BUF_BYTES;

	init_completion(&d->done);
	mutex_init(&d->submit_lock);
	ret = d->ops->irq_setup(d, dmaeng_isr);
	if (ret) {
		dev_err(d->dev, "irq_setup failed: %d\n", ret);
		goto err_free_buf;
	}

	dmaeng_hw_init(d);

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
