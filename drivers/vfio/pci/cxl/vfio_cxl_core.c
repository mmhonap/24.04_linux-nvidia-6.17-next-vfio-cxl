// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO CXL Core - Common infrastructure for CXL Type-2 device variant drivers
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 *
 * This module provides common functionality for VFIO variant drivers that
 * support CXL Type-2 devices (cache-coherent accelerators with attached memory).
 */

#include <linux/vfio_pci_core.h>
#include <linux/vfio_cxl_core.h>
#include <linux/pci.h>
#include <cxl/cxl.h>
#include <cxl/pci.h>

#include "../vfio_pci_priv.h"

MODULE_IMPORT_NS("CXL");

static int vfio_cxl_create_device_state(struct vfio_pci_core_device *vdev,
					u16 dvsec)
{
	struct pci_dev *pdev = vdev->pdev;
	struct device *dev = &pdev->dev;
	struct vfio_pci_cxl_state *cxl;
	u16 cap_word;

	/*
	 * The devm allocation for the CXL state remains for the entire time
	 * the PCI device is bound to vfio-pci. From successful CXL init
	 * in probe until the device is released on unbind.
	 * No extra explicit free is needed; devm handles it when
	 * pdev->dev is released.
	 */
	vdev->cxl = devm_cxl_dev_state_create(dev,
					       CXL_DEVTYPE_DEVMEM,
					       pdev->dev.id, dvsec,
					       struct vfio_pci_cxl_state,
					       cxlds, false);
	if (!vdev->cxl) {
		pci_err(pdev, "Failed to create CXL device state\n");
		return -ENOMEM;
	}

	cxl = vdev->cxl;
	cxl->dvsec = dvsec;

	pci_read_config_word(pdev, dvsec + 0xa, &cap_word);  // CXL Capability
	pci_dbg(pdev, "vfio_cxl: CXL Capability Register: 0x%04x\n", cap_word);
	pci_dbg(pdev, "vfio_cxl:    CXL.cache: %s\n",
		(cap_word & BIT(0)) ? "yes" : "no");
	pci_dbg(pdev, "vfio_cxl:    CXL.io: %s\n",
		(cap_word & BIT(1)) ? "yes" : "no");
	pci_dbg(pdev, "vfio_cxl:    CXL.mem: %s\n",
		(cap_word & BIT(2)) ? "yes" : "no");
	pci_dbg(pdev, "vfio_cxl:    HDM Count: %d\n",
		(cap_word >> 4) & 0x3);

	return 0;
}

static int vfio_cxl_find_bar(struct pci_dev *pdev, resource_size_t hpa, u8 *bar,
			     loff_t *bar_offset, size_t size)
{
	resource_size_t start, end;
	unsigned long flags;
	int index, i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		index = i + PCI_STD_RESOURCES;
		flags = pci_resource_flags(pdev, index);
		start = pci_resource_start(pdev, index);
		end = pci_resource_end(pdev, index);

		/* Skip unprogrammed or empty BARs */
		if (!start || !pci_resource_len(pdev, index) ||
		    hpa < start || hpa + size - 1 > end) {
			if (flags & IORESOURCE_MEM_64)
				i++;
			continue;
		}

		*bar = index;
		*bar_offset = hpa - start;
		return 0;
	}

	return -ENODEV;
}

static int vfio_cxl_setup_regs(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	struct cxl_register_map *map = &cxl->cxlds.reg_map;
	resource_size_t offset, bar_offset, hpa;
	struct pci_dev *pdev = vdev->pdev;
	void __iomem *base;
	resource_size_t size;
	u32 count;
	int ret;
	u8 bar;

	if (WARN_ON_ONCE(!pci_is_enabled(pdev)))
		return -EINVAL;

	/* Find component register block via Register Locator DVSEC */
	ret = cxl_find_regblock(pdev, CXL_REGLOC_RBI_COMPONENT, map);
	if (ret)
		return ret;

	pci_dbg(pdev,
		"vfio_cxl: Found component regs at 0x%llx, max_size=0x%llx\n",
		 (u64)map->resource, (u64)map->max_size);

	/* Temporarily map the register block */
	base = ioremap(map->resource, map->max_size);
	if (!base)
		return -ENOMEM;

	pci_dbg(pdev,
		"vfio_cxl: Mapped component regs to virtual addr %p\n", base);

	/* Probe component register capabilities */
	cxl_probe_component_regs(&pdev->dev, base, &map->component_map);

	pci_dbg(pdev, "vfio_cxl: After probe - hdm_decoder.valid=%d\n",
		map->component_map.hdm_decoder.valid);

	/* Unmap immediately */
	iounmap(base);

	/* Check if HDM decoder was found */
	if (!map->component_map.hdm_decoder.valid)
		return -ENODEV;

	pci_dbg(pdev,
		"vfio_cxl: HDM decoder at offset=0x%lx, size=0x%lx\n",
		map->component_map.hdm_decoder.offset,
		map->component_map.hdm_decoder.size);

	/* Get HDM register info */
	ret = cxl_get_hdm_reg_info(&cxl->cxlds, &count, &offset, &size);
	if (ret)
		return ret;

	pci_dbg(pdev,
		"vfio_cxl: HDM info - count=%u, offset=0x%llx, size=0x%llx\n",
		 count, offset, size);

	if (!count || !size)
		return -ENODEV;

	cxl->hdm_count = count;
	cxl->hdm_reg_offset = offset;
	cxl->hdm_reg_size = size;

	/* HPA of component register block */
	hpa = map->resource;

	ret = vfio_cxl_find_bar(pdev, hpa, &bar, &bar_offset,
				CXL_COMPONENT_REG_BLOCK_SIZE);
	if (ret)
		return ret;

	cxl->comp_reg_bar = bar;
	cxl->comp_reg_offset = bar_offset;
	cxl->comp_reg_size = CXL_COMPONENT_REG_BLOCK_SIZE;

	pci_dbg(pdev,
		"vfio_cxl: component regs: BAR%d offset 0x%llx size 0x%lx\n",
		cxl->comp_reg_bar, cxl->comp_reg_offset, cxl->comp_reg_size);

	return 0;
}

/**
 * vfio_pci_cxl_detect_and_init - Detect and initialize CXL Type-2 device
 * @vdev: VFIO PCI device
 *
 * All CXL init runs at probe only. Full init: detection, state, regs, region
 * size from sysfs/HDM, capacity, add_memdev, region, register VFIO region.
 * Called only from vfio_pci_core_register_device().
 */

void vfio_pci_cxl_detect_and_init(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_cxl_state *cxl;
	u16 dvsec;
	int ret;

	if (!pcie_is_cxl(pdev)) {
		pci_dbg(pdev, "Not a CXL device\n");
		return;
	}

	dvsec = pci_find_dvsec_capability(pdev,
					  PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec) {
		pci_err(pdev, "CXL DVSEC not found\n");
		return;
	}

	pci_dbg(pdev,
		"vfio_cxl: Initializing CXL Type-2 device (DVSEC at 0x%x)\n",
		dvsec);

	ret = vfio_cxl_create_device_state(vdev, dvsec);
	if (ret)
		return;

	cxl = vdev->cxl;

	/*
	 * Required for ioremap of the component register block and
	 * calls to cxl_probe_component_regs().
	 */
	ret = pci_enable_device_mem(pdev);
	if (ret) {
		pci_err(pdev, "Failed to enable PCI device: %d\n", ret);
		goto failed;
	}

	ret = vfio_cxl_setup_regs(vdev);
	if (ret) {
		pci_err(pdev, "Failed to setup CXL registers: %d\n", ret);
		pci_disable_device(pdev);
		goto failed;
	}

	pci_disable_device(pdev);

	pci_dbg(pdev, "vfio_cxl: Component registers probed successfully\n");

	pci_info(pdev, "CXL Type-2 device initialized successfully\n");

	return;

failed:
	vdev->cxl = NULL;
}
EXPORT_SYMBOL_GPL(vfio_pci_cxl_detect_and_init);

void vfio_pci_cxl_cleanup(struct vfio_pci_core_device *vdev)
{
	if (!vdev->cxl)
		return;
}
EXPORT_SYMBOL_GPL(vfio_pci_cxl_cleanup);
