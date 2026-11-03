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
	__u32 flags;		/* must be zero, except DMAENG_XFER_FAULT_* */
	__u64 lat_ns;		/* OUT: driver-measured submit to wake */
};

/*
 * Corrupt the descriptor to exercise device errors the driver otherwise
 * prevents. Refused unless the driver is built with FAULT_INJECT=1.
 */
#define DMAENG_XFER_FAULT_BAD_ADDR	(1U << 0)	/* unmapped src */
#define DMAENG_XFER_FAULT_RSVD		(1U << 1)	/* _rsvd non-zero */
#define DMAENG_XFER_FAULT_NO_IRQ	(1U << 2)	/* no IRQ: lost wake-up */

struct dmaeng_info {
	__u64 buf_bytes;	/* size of the mmap'able buffer */
	__u32 ring_entries;
	__u32 max_xfer;		/* bytes per transfer */
};

#define DMAENG_IOC_MAGIC	'D'
#define DMAENG_SUBMIT		_IOWR(DMAENG_IOC_MAGIC, 1, struct dmaeng_xfer)
#define DMAENG_GET_INFO		_IOR(DMAENG_IOC_MAGIC, 2, struct dmaeng_info)

#endif /* DMAENG_UAPI_H */
