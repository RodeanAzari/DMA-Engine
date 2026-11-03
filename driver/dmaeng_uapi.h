/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace ABI of /dev/dmaeng, shared by the driver and userspace.
 *
 * Transfers name offsets into the buffer mmap'd from the device, never
 * addresses, so userspace cannot point the engine at arbitrary memory.
 */
#ifndef DMAENG_UAPI_H
#define DMAENG_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct dmaeng_xfer {
	__u64 src_off;		/* offset into the mmap'd buffer */
	__u64 dst_off;
	__u32 len;
	__u32 flags;		/* DMAENG_XFER_* */
	__u64 lat_ns;		/* OUT: driver-measured submit to wake */
	__u32 desc_idx;		/* OUT: ring slot used; poll it after NOWAIT */
	__u32 _pad;		/* must be zero */
};

/* Default is block on the interrupt. Both of these send no interrupt. */
#define DMAENG_XFER_POLL		(1U << 0)	/* spin on OWN in the kernel */
#define DMAENG_XFER_NOWAIT		(1U << 1)	/* return; caller polls ring */

/*
 * Corrupt the transfer to exercise device errors the driver otherwise
 * prevents. Refused unless the driver is built with FAULT_INJECT=1.
 */
#define DMAENG_XFER_FAULT_BAD_ADDR	(1U << 8)	/* unmapped src */
#define DMAENG_XFER_FAULT_RSVD		(1U << 9)	/* _rsvd non-zero */
#define DMAENG_XFER_FAULT_NO_IRQ	(1U << 10)	/* no IRQ: lost wake-up */
#define DMAENG_XFER_FAULT_NO_OWN	(1U << 11)	/* OWN clear at HEAD */
#define DMAENG_XFER_FAULT_RING_ALIGN	(1U << 12)	/* misaligned RING_BASE */
#define DMAENG_XFER_FAULT_RING_ADDR	(1U << 13)	/* unmapped RING_BASE */

/*
 * mmap regions, selected by page offset. The ring is read-only: userspace
 * may watch OWN and status but never submit, the doorbell stays in the kernel.
 */
#define DMAENG_MMAP_PGOFF_BUF		0
#define DMAENG_MMAP_PGOFF_RING		1

struct dmaeng_info {
	__u64 buf_bytes;	/* size of the mmap'able buffer */
	__u32 ring_entries;
	__u32 max_xfer;		/* bytes per transfer */
};

#define DMAENG_IOC_MAGIC	'D'
#define DMAENG_SUBMIT		_IOWR(DMAENG_IOC_MAGIC, 1, struct dmaeng_xfer)
#define DMAENG_GET_INFO		_IOR(DMAENG_IOC_MAGIC, 2, struct dmaeng_info)

#endif /* DMAENG_UAPI_H */
