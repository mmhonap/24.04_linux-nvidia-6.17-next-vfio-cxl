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

/*
 * CXL DVSEC for CXL Devices - register offsets within the DVSEC
 * (CXL 2.0+ 8.1.3).
 * Offsets are relative to the DVSEC capability base (cxl->dvsec).
 */
#define CXL_DVSEC_HEADER2_OFFSET		0x4
#define CXL_DVSEC_HEADER3_OFFSET		0x8
#define CXL_DVSEC_CAPABILITY_OFFSET		0xa
#define CXL_DVSEC_CONTROL_OFFSET		0xc
#define CXL_DVSEC_STATUS_OFFSET			0xe
#define CXL_DVSEC_CONTROL2_OFFSET		0x10
#define CXL_DVSEC_STATUS2_OFFSET		0x12
#define CXL_DVSEC_LOCK_OFFSET			0x14
#define CXL_DVSEC_CAPABILITY2_OFFSET		0x16
#define CXL_DVSEC_RANGE1_SIZE_HIGH_OFFSET	0x18
#define CXL_DVSEC_RANGE1_SIZE_LOW_OFFSET	0x1c
#define CXL_DVSEC_RANGE1_BASE_HIGH_OFFSET	0x20
#define CXL_DVSEC_RANGE1_BASE_LOW_OFFSET	0x24
#define CXL_DVSEC_RANGE2_SIZE_HIGH_OFFSET	0x28
#define CXL_DVSEC_RANGE2_SIZE_LOW_OFFSET	0x2c
#define CXL_DVSEC_RANGE2_BASE_HIGH_OFFSET	0x30
#define CXL_DVSEC_RANGE2_BASE_LOW_OFFSET	0x34
#define CXL_DVSEC_CAPABILITY3_OFFSET		0x38

/* CXL Control / Status / Lock - bit definitions */
#define CXL_CTRL_LOCK_BIT			BIT(0)
#define CXL_CTRL_CXL_IO_ENABLE_BIT		BIT(1)
#define CXL_CTRL2_INITIATE_CXL_RESET_BIT	BIT(2)
#define CXL_CAP3_VOLATILE_HDM_BIT		BIT(2)	/* Cap3 */
#define CXL_STATUS2_RW1CS_BIT			BIT(3)
#define CXL_CAP3_P2P_BIT			BIT(4)
#define CXL_CAP2_MODIFIED_COMPLETION_BIT	BIT(6)
#define CXL_STATUS_RW1C_BIT			BIT(14)
#define CXL_CTRL_RESERVED_MASK			(BIT(13) | BIT(15))
#define CXL_CTRL_P2P_REV_MASK			BIT(12)
#define CXL_STATUS_RESERVED_MASK		(GENMASK(13, 0) | BIT(15))
#define CXL_CTRL2_RESERVED_MASK			(GENMASK(15, 6) | BIT(1) | BIT(2))
#define CXL_CTRL2_HW_BITS_MASK			(BIT(0) | BIT(1) | BIT(3))
#define CXL_CTRL2_VOLATILE_HDM_REV_MASK		BIT(4)
#define CXL_CTRL2_MODIFIED_COMP_REV_MASK	BIT(5)
#define CXL_LOCK_RESERVED_MASK			GENMASK(15, 1)
#define CXL_BASE_LO_RESERVED_MASK		GENMASK(27, 0)

#endif /* __LINUX_VFIO_CXL_CORE_H */
