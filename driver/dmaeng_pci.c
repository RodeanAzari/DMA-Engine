// SPDX-License-Identifier: GPL-2.0
/*
 * PCI backend: the dmaeng device reached through BAR0 and one MSI vector.
 * The ring, char device, ioctl and mmap all live in the core.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "dmaeng_core.h"
#include "dmaeng_regs.h"

#define DMAENG_PCI_VENDOR_ID	0x1234
#define DMAENG_PCI_DEVICE_ID	0xDEAD
#define DMAENG_BAR		0

struct dmaeng_pci {
	struct dmaeng_dev d;
	struct pci_dev *pdev;
	void __iomem *regs;
};

static u32 dmaeng_pci_reg_read(struct dmaeng_dev *d, u32 off)
{
	struct dmaeng_pci *p = d->priv;

	return readl(p->regs + off);
}

static void dmaeng_pci_reg_write(struct dmaeng_dev *d, u32 off, u32 val)
{
	struct dmaeng_pci *p = d->priv;

	writel(val, p->regs + off);
}

static int dmaeng_pci_irq_setup(struct dmaeng_dev *d, irq_handler_t handler)
{
	struct dmaeng_pci *p = d->priv;

	return request_irq(pci_irq_vector(p->pdev, 0), handler, 0, DRV_NAME, d);
}

static void dmaeng_pci_irq_teardown(struct dmaeng_dev *d)
{
	struct dmaeng_pci *p = d->priv;

	free_irq(pci_irq_vector(p->pdev, 0), d);
}

static const struct dmaeng_backend_ops dmaeng_pci_ops = {
	.reg_read	= dmaeng_pci_reg_read,
	.reg_write	= dmaeng_pci_reg_write,
	.irq_setup	= dmaeng_pci_irq_setup,
	.irq_teardown	= dmaeng_pci_irq_teardown,
};

static int dmaeng_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct dmaeng_pci *p;
	int ret;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->pdev = pdev;
	pci_set_drvdata(pdev, p);

	ret = pci_enable_device(pdev);
	if (ret)
		goto err_free;

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret)
		goto err_disable;

	p->regs = pci_iomap(pdev, DMAENG_BAR, 0);
	if (!p->regs) {
		ret = -ENOMEM;
		goto err_release;
	}

	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto err_clear_master;

	/* MSI only: no fallback, or a broken setup would silently never fire */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		goto err_clear_master;

	p->d.dev = &pdev->dev;
	p->d.ops = &dmaeng_pci_ops;
	p->d.priv = p;

	ret = dmaeng_core_add(&p->d);
	if (ret)
		goto err_free_vectors;

	return 0;

err_free_vectors:
	pci_free_irq_vectors(pdev);
err_clear_master:
	pci_clear_master(pdev);
	pci_iounmap(pdev, p->regs);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(p);
	return ret;
}

static void dmaeng_pci_remove(struct pci_dev *pdev)
{
	struct dmaeng_pci *p = pci_get_drvdata(pdev);

	dmaeng_core_remove(&p->d);
	pci_free_irq_vectors(pdev);
	pci_clear_master(pdev);
	pci_iounmap(pdev, p->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(p);
}

static const struct pci_device_id dmaeng_pci_ids[] = {
	{ PCI_DEVICE(DMAENG_PCI_VENDOR_ID, DMAENG_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dmaeng_pci_ids);

static struct pci_driver dmaeng_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= dmaeng_pci_ids,
	.probe		= dmaeng_pci_probe,
	.remove		= dmaeng_pci_remove,
};
module_pci_driver(dmaeng_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dmaeng driver, PCI backend");
