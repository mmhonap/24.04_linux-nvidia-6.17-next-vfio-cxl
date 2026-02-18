/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Common infrastructure for CXL Type-2 device variant drivers
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#ifndef __LINUX_VFIO_CXL_CORE_H
#define __LINUX_VFIO_CXL_CORE_H

#include <cxl/cxl.h>
#include <linux/types.h>

struct vfio_pci_core_device;

struct vfio_emulated_regblock {
	struct range range;
	ssize_t (*read)(struct vfio_pci_core_device *vdev, void *buf,
			u64 offset, u64 size);
	ssize_t (*write)(struct vfio_pci_core_device *vdev, void *buf,
			 u64 offset, u64 size);
	struct list_head list;
};

/* CXL device state embedded in vfio_pci_core_device */
struct vfio_pci_cxl_state {
	struct cxl_dev_state         cxlds;
	struct cxl_memdev           *cxlmd;
	struct cxl_root_decoder     *cxlrd;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region           *region;
	resource_size_t              region_hpa;
	size_t                       region_size;
	void __iomem                *region_vaddr;
	resource_size_t              hdm_reg_offset;
	size_t                       hdm_reg_size;
	resource_size_t              comp_reg_offset;
	size_t                       comp_reg_size;
	void                        *initial_comp_reg_virt;
	void                        *comp_reg_virt;
	void                        *initial_config_virt;
	void                        *config_virt;
	size_t                       config_size;
	struct list_head             config_regblocks_head;
	struct list_head             mmio_regblocks_head;
	size_t                       dpa_size;
	u32                          hdm_count;
	u16                          dvsec;
	u8                           comp_reg_bar;
	bool                         precommitted;
};

/* Register access sizes */
#define CXL_REG_SIZE_WORD 2
#define CXL_REG_SIZE_DWORD 4

/* HDM Decoder - register offsets (CXL 2.0 8.2.5.19) */
#define CXL_HDM_DECODER_GLOBAL_CTRL_OFFSET	0x4
#define CXL_HDM_DECODER_FIRST_BLOCK_OFFSET	0x10
#define CXL_HDM_DECODER_BLOCK_STRIDE		0x20
#define CXL_HDM_DECODER_N_BASE_LOW_OFFSET	0x0
#define CXL_HDM_DECODER_N_BASE_HIGH_OFFSET	0x4
#define CXL_HDM_DECODER_N_SIZE_LOW_OFFSET	0x8
#define CXL_HDM_DECODER_N_SIZE_HIGH_OFFSET	0xc
#define CXL_HDM_DECODER_N_CTRL_OFFSET		0x10
#define CXL_HDM_DECODER_N_TARGET_LIST_LOW_OFFSET	0x14
#define CXL_HDM_DECODER_N_TARGET_LIST_HIGH_OFFSET 0x18
#define CXL_HDM_DECODER_N_REV_OFFSET		0x1c

/* HDM Decoder Global Capability / Control - bit definitions */
#define CXL_HDM_CAP_POISON_ON_DECODE_ERR_BIT	BIT(10)
#define CXL_HDM_CAP_UIO_SUPPORTED_BIT		BIT(13)
/* HDM Decoder N Control */
#define CXL_HDM_DECODER_CTRL_COMMIT_LOCK_BIT	BIT(8)
#define CXL_HDM_DECODER_CTRL_COMMIT_BIT		BIT(9)
#define CXL_HDM_DECODER_CTRL_COMMITTED_BIT	BIT(10)
#define CXL_HDM_DECODER_CTRL_RO_BITS_MASK	(BIT(10) | BIT(11))
#define CXL_HDM_DECODER_CTRL_RESERVED_MASK	(BIT(15) | GENMASK(31, 28))
#define CXL_HDM_DECODER_CTRL_DEVICE_BITS_RO	BIT(12)
#define CXL_HDM_DECODER_CTRL_DEVICE_RESERVED	(GENMASK(19, 16) | GENMASK(23, 20))
#define CXL_HDM_DECODER_CTRL_UIO_RESERVED	(BIT(14) | GENMASK(27, 24))
#define CXL_HDM_DECODER_BASE_LO_RESERVED_MASK	GENMASK(27, 0)
#define CXL_HDM_DECODER_GLOBAL_CTRL_RESERVED_MASK GENMASK(31, 2)
#define CXL_HDM_DECODER_GLOBAL_CTRL_POISON_EN_BIT BIT(0)

#endif /* __LINUX_VFIO_CXL_CORE_H */
