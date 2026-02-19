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

static void
vfio_cxl_read_committed_decoder_size(struct vfio_pci_cxl_state *cxl,
				     struct pci_dev *pdev,
				     resource_size_t *size)
{
	u32 ctrl, size_low, size_high;
	resource_size_t decoder_size;
	void __iomem *hdm_base;

	hdm_base = ioremap(cxl->cxlds.reg_map.resource +
			   cxl->hdm_reg_offset, cxl->hdm_reg_size);
	if (!hdm_base)
		return;

	/* Read decoder 0 control register to check if committed */
	ctrl = readl(hdm_base + CXL_HDM_DECODER0_CTRL_OFFSET(0));

	if (ctrl & CXL_HDM_DECODER0_CTRL_COMMITTED) {
		/* Decoder is committed, read its size */
		size_high = readl(hdm_base +
				  CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(0));
		size_low = readl(hdm_base +
				 CXL_HDM_DECODER0_SIZE_LOW_OFFSET(0));

		/*
		 * CXL 3.1 8.2.4.20.5 CXL HDM Decoder n Size Low Register
		 * Size is in units of 256MB, low bits [27:0] are reserved.
		 */
		decoder_size = ((resource_size_t)size_high << 32) |
			(size_low & GENMASK(31, 28));

		*size = decoder_size;
	} else {
		pci_err(pdev, "vfio_cxl: HDM decoder 0 is not committed"
			" (ctrl=0x%08x)\n", ctrl);
	}

	iounmap(hdm_base);

	return;
}

static int vfio_cxl_create_memdev(struct vfio_pci_core_device *vdev,
				  resource_size_t capacity)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	struct pci_dev *pdev = vdev->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	ret = cxl_set_capacity(&cxl->cxlds, capacity);
	if (ret) {
		pci_err(pdev, "Failed to set capacity: %d\n", ret);
		return ret;
	}

	pci_dbg(pdev, "Device capacity: %llu MB (from %s)\n",
		capacity >> 20,
		cxl->precommitted ? "committed decoder" : "sysfs");

	cxl->cxlmd = devm_cxl_add_memdev(dev, &cxl->cxlds, NULL);
	if (IS_ERR(cxl->cxlmd)) {
		pci_err(pdev, "Failed to add CXL memdev: %ld\n",
			PTR_ERR(cxl->cxlmd));
		return PTR_ERR(cxl->cxlmd);
	}

	return 0;
}

static int vfio_cxl_allocate_hpa(struct vfio_pci_cxl_state *cxl,
				 struct pci_dev *pdev, resource_size_t size)
{
	resource_size_t max_size;

	cxl->cxlrd = cxl_get_hpa_freespace(cxl->cxlmd, 1,
					   CXL_DECODER_F_RAM |
					   CXL_DECODER_F_TYPE2,
					   &max_size);
	if (IS_ERR(cxl->cxlrd)) {
		pci_err(pdev, "Failed to get HPA free space\n");
		return PTR_ERR(cxl->cxlrd);
	}

	if (max_size < size) {
		pci_err(pdev,
			"Insufficient HPA space: need %llu, available %pa\n",
			size, &max_size);
		cxl_put_root_decoder(cxl->cxlrd);
		cxl->cxlrd = NULL;
		return -ENOSPC;
	}

	pci_dbg(pdev, "vfio_cxl: Allocated HPA space: %llu bytes\n", max_size);
	return 0;
}

static int vfio_cxl_allocate_dpa(struct vfio_pci_cxl_state *cxl,
				 struct pci_dev *pdev, resource_size_t size)
{
	cxl->cxled = cxl_request_dpa(cxl->cxlmd, CXL_PARTMODE_RAM, size);
	if (IS_ERR(cxl->cxled)) {
		pci_err(pdev, "Failed to allocate DPA\n");
		return PTR_ERR(cxl->cxled);
	}

	pci_dbg(pdev, "vfio_cxl: Allocated DPA: %llu bytes\n", size);
	return 0;
}

static int vfio_cxl_create_region(struct vfio_pci_cxl_state *cxl,
				  struct pci_dev *pdev)
{
	cxl->region = cxl_create_region(cxl->cxlrd, &cxl->cxled, 1);
	if (IS_ERR(cxl->region)) {
		pci_err(pdev, "Failed to create CXL region\n");
		return PTR_ERR(cxl->region);
	}

	pci_dbg(pdev, "vfio_cxl: Created CXL region\n");
	return 0;
}

int vfio_cxl_create_cxl_region(struct vfio_pci_core_device *vdev, resource_size_t size)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	struct pci_dev *pdev = vdev->pdev;
	int ret;

	if (cxl->precommitted)
		return 0;

	/* Not pre-committed, need to allocate resources */
	ret = vfio_cxl_allocate_hpa(cxl, pdev, size);
	if (ret)
		return ret;

	ret = vfio_cxl_allocate_dpa(cxl, pdev, size);
	if (ret)
		goto err_free_hpa;

	ret = vfio_cxl_create_region(cxl, pdev);
	if (ret)
		goto err_free_dpa;

	return 0;

err_free_dpa:
	cxl_dpa_free(cxl->cxled);
err_free_hpa:
	if (cxl->cxlrd)
		cxl_put_root_decoder(cxl->cxlrd);

	return ret;
}
EXPORT_SYMBOL_GPL(vfio_cxl_create_cxl_region);

void vfio_cxl_destroy_cxl_region(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	if (!cxl->region)
		return;

	cxl_decoder_detach(NULL, cxl->cxled, 0, DETACH_INVALIDATE);

	cxl->region = NULL;

	if (cxl->precommitted)
		return;

	cxl_dpa_free(cxl->cxled);
	cxl_put_root_decoder(cxl->cxlrd);
}
EXPORT_SYMBOL_GPL(vfio_cxl_destroy_cxl_region);

static int vfio_cxl_create_region_helper(struct vfio_pci_core_device *vdev,
					 resource_size_t capacity)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	struct pci_dev *pdev = vdev->pdev;
	int ret;

	if (cxl->precommitted) {
		cxl->cxled = cxl_get_committed_decoder(cxl->cxlmd,
						       &cxl->region);
		if (IS_ERR(cxl->cxled))
			return PTR_ERR(cxl->cxled);
	} else {
		ret = vfio_cxl_create_cxl_region(vdev, capacity);
		if (ret)
			return ret;
	}

	if (cxl->region) {
		struct range range;

		ret = cxl_get_region_range(cxl->region, &range);
		if (ret)
			goto failed;

		cxl->region_hpa = range.start;
		cxl->region_size = range_len(&range);

		pci_dbg(pdev, "Precommitted decoder: HPA 0x%llx "
			"size %lu MB\n",
			cxl->region_hpa, cxl->region_size >> 20);
	} else {
		ret = -ENODEV;
		goto failed;
	}

	return 0;

failed:
	vfio_cxl_destroy_cxl_region(vdev);
	return ret;
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
	resource_size_t capacity = 0;
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

	cxl->cxlds.media_ready = !cxl_await_range_active(&cxl->cxlds);
	if (!cxl->cxlds.media_ready) {
		pci_err(pdev, "CXL media not ready\n");
		goto failed;
	}

	/*
	 * Read committed decoder size BEFORE add_memdev
	 * We need capacity for cxl_set_capacity() before devm_cxl_add_memdev(),
	 * but the committed region size is only known after the CXL core
	 * enumerates decoders.
	 * Solution: read the same hardware the core would use (HDM decoder)
	 * directly here, and use that as capacity. CXL core will see the
	 * same values when it enumerates decoders inside add_memdev.
	 */
	vfio_cxl_read_committed_decoder_size(cxl, pdev, &capacity);
	if (capacity > 0) {
		cxl->precommitted = true;
		cxl->dpa_size = capacity;
	}

	if (capacity == 0) {
		/*
		 * TODO: Add handling for devices which do not have
		 * firmware pre-committed decoders
		 */
		pci_info(pdev, "Uncommitted region size must be configured "
			 "via sysfs before bind\n");
		goto failed;
	}

	ret = vfio_cxl_create_memdev(vdev, capacity);
	if (ret) {
		pci_err(pdev, "Failed to setup CXL memory device: %d\n", ret);
		goto failed;
	}

	ret = vfio_cxl_create_region_helper(vdev, capacity);
	if (ret) {
		pci_err(pdev, "Failed to create CXL region: %d\n", ret);
		goto failed;
	}

	pci_info(pdev, "CXL Type-2 device initialized successfully\n");

	return;

failed:
	vdev->cxl = NULL;
}
EXPORT_SYMBOL_GPL(vfio_pci_cxl_detect_and_init);

void vfio_pci_cxl_cleanup(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	if (!cxl || !cxl->region)
		return;

	cxl_decoder_detach(NULL, cxl->cxled, 0, DETACH_INVALIDATE);

	/* Free resources only if we allocated them */
	if (!cxl->precommitted) {
		cxl_dpa_free(cxl->cxled);
		cxl_put_root_decoder(cxl->cxlrd);
	}

	unregister_region(cxl->region);

	cxl->region = NULL;
}
EXPORT_SYMBOL_GPL(vfio_pci_cxl_cleanup);
