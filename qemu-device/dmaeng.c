// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dmaeng: descriptor-ring DMA engine as a QEMU PCIe device.
 *
 * Register map and descriptor layout come from the generated dmaeng_regs.h
 * (spec/dmaeng.yaml). Behaviour follows CLAUDE.md section 5, which the
 * future RTL must match, so every rule the RTL inherits is honoured here
 * even where QEMU would forgive breaking it.
 *
 *   -device dmaeng                           complete inside the doorbell write
 *   -device dmaeng,completion-delay-ns=5000  complete later, one per timer tick
 *   -device dmaeng,trace-file=<path>         append a golden trace for the RTL
 *
 * Trace records, one per line, data in hex; format in qemu-device/PATCH.md.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"

#include "dmaeng_regs.h"

#define TYPE_DMAENG	"dmaeng"
#define DMAENG_DEVICE_ID	0xDEAD
#define DMAENG_BAR_BYTES	4096
#define DMAENG_PCIE_CAP_OFF	0x80

OBJECT_DECLARE_SIMPLE_TYPE(DmaengState, DMAENG)

struct DmaengState {
	PCIDevice pdev;
	MemoryRegion mmio;
	QEMUTimer timer;
	uint64_t completion_delay_ns;	/* property; 0 = synchronous */
	uint8_t *xfer_buf;		/* MAX_XFER bytes, holds a copy in flight */
	char *trace_path;		/* property; NULL = no trace */
	FILE *trace;
	uint8_t *trace_buf;		/* MAX_XFER bytes, destination snapshots */

	uint32_t ctrl;
	uint32_t status;
	uint32_t ring_base_lo;
	uint32_t ring_base_hi;
	uint32_t ring_size;
	uint32_t head;
	uint32_t tail;
	uint32_t irq_status;
	uint32_t irq_mask;
	uint32_t err_code;
	bool irq_level;			/* irq_status & irq_mask != 0 */
};

static void G_GNUC_PRINTF(2, 3) dmaeng_trace(DmaengState *s, const char *fmt, ...)
{
	va_list ap;

	if (!s->trace)
		return;
	va_start(ap, fmt);
	vfprintf(s->trace, fmt, ap);
	va_end(ap);
}

static void dmaeng_trace_hex(DmaengState *s, const char *tag,
			     const uint8_t *data, uint32_t len)
{
	uint32_t i;

	fprintf(s->trace, "%s ", tag);
	for (i = 0; i < len; i++)
		fprintf(s->trace, "%02x", data[i]);
	fputc('\n', s->trace);
}

/* Spec 5.4: the interrupt is the level irq_status & irq_mask; MSI on 0->1 */
static void dmaeng_update_irq(DmaengState *s)
{
	bool level = s->irq_status & s->irq_mask;

	if (level && !s->irq_level && msi_enabled(&s->pdev)) {
		dmaeng_trace(s, "MSI\n");
		msi_notify(&s->pdev, 0);
	}
	s->irq_level = level;
}

/* Spec 5.5: record the error and stop until CTRL.RESET */
static void dmaeng_error(DmaengState *s, uint32_t code)
{
	dmaeng_trace(s, "ERROR code=0x%x\n", code);
	s->err_code = code;
	s->status |= DMAENG_STATUS_ERROR;
	s->irq_status |= DMAENG_IRQ_STATUS_ERROR;
	dmaeng_update_irq(s);
}

static bool dmaeng_has_work(DmaengState *s)
{
	return (s->ctrl & DMAENG_CTRL_ENABLE) &&
	       !(s->status & DMAENG_STATUS_ERROR) && s->head != s->tail;
}

static void dmaeng_update_busy(DmaengState *s)
{
	if (dmaeng_has_work(s))
		s->status |= DMAENG_STATUS_BUSY;
	else
		s->status &= ~DMAENG_STATUS_BUSY;
}

/* Returns a spec 5.5 error code. The temporary buffer makes this one step. */
static uint32_t dmaeng_copy(DmaengState *s, uint64_t src, uint64_t dst,
			    uint32_t len)
{
	if (pci_dma_read(&s->pdev, src, s->xfer_buf, len) != MEMTX_OK)
		return DMAENG_ERR_DMA_FAIL;
	if (s->trace) {
		dmaeng_trace_hex(s, "SRC", s->xfer_buf, len);
		if (pci_dma_read(&s->pdev, dst, s->trace_buf, len) == MEMTX_OK)
			dmaeng_trace_hex(s, "DST_BEFORE", s->trace_buf, len);
	}
	if (pci_dma_write(&s->pdev, dst, s->xfer_buf, len) != MEMTX_OK)
		return DMAENG_ERR_DMA_FAIL;
	if (s->trace &&
	    pci_dma_read(&s->pdev, dst, s->trace_buf, len) == MEMTX_OK)
		dmaeng_trace_hex(s, "DST_AFTER", s->trace_buf, len);
	return DMAENG_ERR_OK;
}

/* Process the descriptor at HEAD (spec 5.4) */
static void dmaeng_step(DmaengState *s)
{
	uint64_t base = (uint64_t)s->ring_base_hi << 32 | s->ring_base_lo;
	uint64_t desc_addr = base + (uint64_t)s->head * DMAENG_DESC_SIZE;
	uint8_t desc[DMAENG_DESC_SIZE];
	uint32_t flags, len, err;
	uint8_t le_status[4], le_flags[4];

	/* Errors before a descriptor is owned touch nothing in the ring */
	if (base % DMAENG_RING_ALIGN || !is_power_of_2(s->ring_size) ||
	    s->ring_size < DMAENG_RING_SIZE_MIN ||
	    s->ring_size > DMAENG_RING_SIZE_MAX) {
		dmaeng_error(s, DMAENG_ERR_BAD_DESC);
		return;
	}
	if (pci_dma_read(&s->pdev, desc_addr, desc, sizeof(desc)) != MEMTX_OK) {
		dmaeng_error(s, DMAENG_ERR_DMA_FAIL);
		return;
	}
	flags = ldl_le_p(desc + DMAENG_DESC_OFF_FLAGS);
	if (!(flags & DMAENG_DESC_OWN)) {
		/* HEAD != TAIL but not ours: driver bug */
		dmaeng_error(s, DMAENG_ERR_BAD_DESC);
		return;
	}

	len = ldl_le_p(desc + DMAENG_DESC_OFF_LEN);
	dmaeng_trace(s, "DESC head=%u addr=0x%" PRIx64 " src=0x%" PRIx64
		     " dst=0x%" PRIx64 " len=%u flags=0x%x rsvd=0x%x\n",
		     s->head, desc_addr, ldq_le_p(desc + DMAENG_DESC_OFF_SRC),
		     ldq_le_p(desc + DMAENG_DESC_OFF_DST), len, flags,
		     ldl_le_p(desc + DMAENG_DESC_OFF_RSVD));
	if (ldl_le_p(desc + DMAENG_DESC_OFF_RSVD))
		err = DMAENG_ERR_BAD_DESC;
	else if (!len || len > DMAENG_MAX_XFER_DEFAULT)
		err = DMAENG_ERR_BAD_LEN;
	else
		err = dmaeng_copy(s, ldq_le_p(desc + DMAENG_DESC_OFF_SRC),
				  ldq_le_p(desc + DMAENG_DESC_OFF_DST), len);

	dmaeng_trace(s, "DONE status=0x%x\n", err);

	/* Spec 5.6 rule 5: status lands before OWN clears */
	stl_le_p(le_status, err);
	stl_le_p(le_flags, flags & ~DMAENG_DESC_OWN);
	if (pci_dma_write(&s->pdev, desc_addr + DMAENG_DESC_OFF_STATUS,
			  le_status, sizeof(le_status)) != MEMTX_OK ||
	    pci_dma_write(&s->pdev, desc_addr + DMAENG_DESC_OFF_FLAGS,
			  le_flags, sizeof(le_flags)) != MEMTX_OK) {
		dmaeng_error(s, DMAENG_ERR_DMA_FAIL);
		return;
	}

	s->head = (s->head + 1) & (s->ring_size - 1);

	if (err) {
		dmaeng_error(s, err);
		return;
	}
	if (flags & DMAENG_DESC_IRQ) {
		s->irq_status |= DMAENG_IRQ_STATUS_COMPLETION;
		dmaeng_update_irq(s);
	}
}

/*
 * Doorbell. Each step advances HEAD or stops on an error, and a stale
 * descriptor has OWN clear, so even a bad TAIL cannot loop forever.
 */
static void dmaeng_kick(DmaengState *s)
{
	dmaeng_update_busy(s);
	if (!dmaeng_has_work(s))
		return;

	if (s->completion_delay_ns) {
		if (!timer_pending(&s->timer))
			timer_mod(&s->timer,
				  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
				  s->completion_delay_ns);
		return;
	}

	while (dmaeng_has_work(s))
		dmaeng_step(s);
	dmaeng_update_busy(s);
}

/* Delayed mode: one descriptor per tick */
static void dmaeng_timer_cb(void *opaque)
{
	DmaengState *s = opaque;

	if (dmaeng_has_work(s))
		dmaeng_step(s);
	dmaeng_kick(s);
}

/* CTRL.RESET and machine reset: clears everything but the PCI config space */
static void dmaeng_reset_state(DmaengState *s)
{
	timer_del(&s->timer);
	s->ctrl = 0;
	s->status = 0;
	s->ring_base_lo = 0;
	s->ring_base_hi = 0;
	s->ring_size = 0;
	s->head = 0;
	s->tail = 0;
	s->irq_status = 0;
	s->irq_mask = 0;
	s->err_code = 0;
	s->irq_level = false;
}

static uint32_t dmaeng_reg_read(DmaengState *s, hwaddr addr)
{
	switch (addr) {
	case DMAENG_REG_ID:
		return DMAENG_ID_MAGIC;
	case DMAENG_REG_VERSION:
		return DMAENG_VERSION;
	case DMAENG_REG_CTRL:
		return s->ctrl;
	case DMAENG_REG_STATUS:
		return s->status;
	case DMAENG_REG_RING_BASE_LO:
		return s->ring_base_lo;
	case DMAENG_REG_RING_BASE_HI:
		return s->ring_base_hi;
	case DMAENG_REG_RING_SIZE:
		return s->ring_size;
	case DMAENG_REG_HEAD:
		return s->head;
	case DMAENG_REG_TAIL:
		return s->tail;
	case DMAENG_REG_IRQ_STATUS:
		return s->irq_status;
	case DMAENG_REG_IRQ_MASK:
		return s->irq_mask;
	case DMAENG_REG_ERR_CODE:
		return s->err_code;
	case DMAENG_REG_MAX_XFER:
		return DMAENG_MAX_XFER_DEFAULT;
	default:
		return 0;
	}
}

static uint64_t dmaeng_mmio_read(void *opaque, hwaddr addr, unsigned int size)
{
	DmaengState *s = opaque;
	uint32_t val;

	if (size != DMAENG_REG_ACCESS_BYTES || addr % DMAENG_REG_ACCESS_BYTES) {
		qemu_log_mask(LOG_GUEST_ERROR,
			      "dmaeng: bad read at 0x%" HWADDR_PRIx " size %u\n",
			      addr, size);
		return 0;
	}

	val = dmaeng_reg_read(s, addr);
	dmaeng_trace(s, "RD 0x%02" HWADDR_PRIx " 0x%08x\n", addr, val);
	return val;
}

static void dmaeng_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
			      unsigned int size)
{
	DmaengState *s = opaque;
	uint32_t val = val64;

	if (size != DMAENG_REG_ACCESS_BYTES || addr % DMAENG_REG_ACCESS_BYTES) {
		qemu_log_mask(LOG_GUEST_ERROR,
			      "dmaeng: bad write at 0x%" HWADDR_PRIx " size %u\n",
			      addr, size);
		return;
	}

	dmaeng_trace(s, "WR 0x%02" HWADDR_PRIx " 0x%08x\n", addr, val);

	switch (addr) {
	case DMAENG_REG_CTRL:
		if (val & DMAENG_CTRL_RESET) {
			dmaeng_reset_state(s);	/* self-clearing */
		} else {
			s->ctrl = val & DMAENG_CTRL_ENABLE;
			dmaeng_kick(s);
		}
		break;
	case DMAENG_REG_RING_BASE_LO:
		s->ring_base_lo = val;
		break;
	case DMAENG_REG_RING_BASE_HI:
		s->ring_base_hi = val;
		break;
	case DMAENG_REG_RING_SIZE:
		s->ring_size = val;
		break;
	case DMAENG_REG_TAIL:
		s->tail = val;
		dmaeng_kick(s);
		break;
	case DMAENG_REG_IRQ_STATUS:
		s->irq_status &= ~val;		/* write-1-to-clear */
		dmaeng_update_irq(s);
		break;
	case DMAENG_REG_IRQ_MASK:
		s->irq_mask = val;
		dmaeng_update_irq(s);
		break;
	default:
		break;				/* read-only or unimplemented */
	}
}

/* Accept every width so the handlers can log and ignore wrong ones (spec 5.2) */
static const MemoryRegionOps dmaeng_mmio_ops = {
	.read = dmaeng_mmio_read,
	.write = dmaeng_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = { .min_access_size = 1, .max_access_size = 8 },
	.impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void dmaeng_realize(PCIDevice *pdev, Error **errp)
{
	DmaengState *s = DMAENG(pdev);
	int ret;

	ret = pcie_endpoint_cap_init(pdev, DMAENG_PCIE_CAP_OFF);
	if (ret < 0) {
		error_setg(errp, "dmaeng: PCIe capability init failed");
		return;
	}
	if (msi_init(pdev, 0, 1, true, false, errp)) {
		pcie_cap_exit(pdev);
		return;
	}

	if (s->trace_path) {
		s->trace = fopen(s->trace_path, "a");
		if (!s->trace) {
			error_setg_file_open(errp, errno, s->trace_path);
			msi_uninit(pdev);
			pcie_cap_exit(pdev);
			return;
		}
		/* QEMU may exit without calling dmaeng_exit(), so lose nothing */
		setvbuf(s->trace, NULL, _IOLBF, 0);
		s->trace_buf = g_malloc(DMAENG_MAX_XFER_DEFAULT);
	}

	s->xfer_buf = g_malloc(DMAENG_MAX_XFER_DEFAULT);
	timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, dmaeng_timer_cb, s);

	memory_region_init_io(&s->mmio, OBJECT(s), &dmaeng_mmio_ops, s,
			      "dmaeng-mmio", DMAENG_BAR_BYTES);
	pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void dmaeng_exit(PCIDevice *pdev)
{
	DmaengState *s = DMAENG(pdev);

	timer_del(&s->timer);
	g_free(s->xfer_buf);
	g_free(s->trace_buf);
	if (s->trace)
		fclose(s->trace);
	msi_uninit(pdev);
	pcie_cap_exit(pdev);
}

static void dmaeng_qdev_reset(DeviceState *dev)
{
	dmaeng_reset_state(DMAENG(dev));
}

static const Property dmaeng_properties[] = {
	DEFINE_PROP_UINT64("completion-delay-ns", DmaengState,
			   completion_delay_ns, 0),
	DEFINE_PROP_STRING("trace-file", DmaengState, trace_path),
};

static void dmaeng_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = dmaeng_realize;
	k->exit = dmaeng_exit;
	k->vendor_id = PCI_VENDOR_ID_QEMU;
	k->device_id = DMAENG_DEVICE_ID;
	k->class_id = PCI_CLASS_SYSTEM_OTHER;
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);
	device_class_set_props(dc, dmaeng_properties);
	device_class_set_legacy_reset(dc, dmaeng_qdev_reset);
}

static const TypeInfo dmaeng_info = {
	.name		= TYPE_DMAENG,
	.parent		= TYPE_PCI_DEVICE,
	.instance_size	= sizeof(DmaengState),
	.class_init	= dmaeng_class_init,
	.interfaces	= (const InterfaceInfo[]) {
		{ INTERFACE_PCIE_DEVICE },
		{ },
	},
};

static void dmaeng_register_types(void)
{
	type_register_static(&dmaeng_info);
}

type_init(dmaeng_register_types)
