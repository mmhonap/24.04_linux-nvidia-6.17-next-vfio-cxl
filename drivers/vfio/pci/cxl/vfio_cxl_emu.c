// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/vfio_pci_core.h>
#include <linux/vfio_cxl_core.h>

#include "../vfio_pci_priv.h"
#include "vfio_cxl_priv.h"

typedef ssize_t reg_handler_t(struct vfio_pci_core_device *vdev, void *buf,
			      u64 offset, u64 size);

static struct vfio_emulated_regblock *
new_reg_block(struct vfio_pci_core_device *vdev, u64 offset, u64 size,
	      reg_handler_t *read, reg_handler_t *write)
{
	struct vfio_emulated_regblock *block;

	block = kzalloc(sizeof(*block), GFP_KERNEL);
	if (!block)
		return ERR_PTR(-ENOMEM);

	block->range.start = offset;
	block->range.end = offset + size - 1;
	block->read = read;
	block->write = write;

	INIT_LIST_HEAD(&block->list);

	return block;
}

static int new_mmio_block(struct vfio_pci_core_device *vdev, u64 offset, u64 size,
			  reg_handler_t *read, reg_handler_t *write)
{
	struct vfio_emulated_regblock *block;
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	block = new_reg_block(vdev, offset, size, read, write);
	if (IS_ERR(block))
		return PTR_ERR(block);

	list_add_tail(&block->list, &cxl->mmio_regblocks_head);
	return 0;
}

static u64 hdm_reg_base(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	return cxl->comp_reg_offset + cxl->hdm_reg_offset;
}

static u64 to_hdm_reg_offset(struct vfio_pci_core_device *vdev, u64 offset)
{
	return offset - hdm_reg_base(vdev);
}

static void *hdm_reg_virt(struct vfio_pci_core_device *vdev, u64 hdm_reg_offset)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	return cxl->comp_reg_virt + cxl->hdm_reg_offset + hdm_reg_offset;
}

static ssize_t virt_hdm_reg_read(struct vfio_pci_core_device *vdev, void *buf,
				 u64 offset, u64 size)
{
	offset = to_hdm_reg_offset(vdev, offset);
	memcpy(buf, hdm_reg_virt(vdev, offset), size);

	return size;
}

static ssize_t virt_hdm_reg_write(struct vfio_pci_core_device *vdev,
				  void *buf, u64 offset, u64 size)
{
	offset = to_hdm_reg_offset(vdev, offset);
	memcpy(hdm_reg_virt(vdev, offset), buf, size);

	return size;
}

static ssize_t virt_hdm_rev_reg_write(struct vfio_pci_core_device *vdev,
				      void *buf, u64 offset, u64 size)
{
	/* Discard writes on reserved registers. */
	return size;
}

static ssize_t hdm_decoder_n_lo_write(struct vfio_pci_core_device *vdev,
				      void *buf, u64 offset, u64 size)
{
	u32 new_val = le32_to_cpu(*(u32 *)buf);

	if (WARN_ON_ONCE(size != CXL_REG_SIZE_DWORD))
		return -EINVAL;

	/* Bit [27:0] are reserved. */
	new_val &= ~CXL_HDM_DECODER_BASE_LO_RESERVED_MASK;

	new_val = cpu_to_le32(new_val);
	offset = to_hdm_reg_offset(vdev, offset);
	memcpy(hdm_reg_virt(vdev, offset), &new_val, size);

	return size;
}

static ssize_t hdm_decoder_global_ctrl_write(struct vfio_pci_core_device *vdev,
					     void *buf, u64 offset, u64 size)
{
	u32 hdm_decoder_global_cap;
	u32 new_val = le32_to_cpu(*(u32 *)buf);

	if (WARN_ON_ONCE(size != CXL_REG_SIZE_DWORD))
		return -EINVAL;

	/* Bit [31:2] are reserved. */
	new_val &= ~CXL_HDM_DECODER_GLOBAL_CTRL_RESERVED_MASK;

	/* Poison On Decode Error Enable bit is 0 and RO if not support. */
	hdm_decoder_global_cap = le32_to_cpu(*(u32 *)hdm_reg_virt(vdev, 0));
	if (!(hdm_decoder_global_cap & CXL_HDM_CAP_POISON_ON_DECODE_ERR_BIT))
		new_val &= ~CXL_HDM_DECODER_GLOBAL_CTRL_POISON_EN_BIT;

	new_val = cpu_to_le32(new_val);
	offset = to_hdm_reg_offset(vdev, offset);
	memcpy(hdm_reg_virt(vdev, offset), &new_val, size);

	return size;
}

static ssize_t hdm_decoder_n_ctrl_write(struct vfio_pci_core_device *vdev,
					void *buf, u64 offset, u64 size)
{
	u32 hdm_decoder_global_cap;
	u32 ro_mask = CXL_HDM_DECODER_CTRL_RO_BITS_MASK;
	u32 rev_mask = CXL_HDM_DECODER_CTRL_RESERVED_MASK;
	u32 new_val = le32_to_cpu(*(u32 *)buf);
	u32 cur_val;

	if (WARN_ON_ONCE(size != CXL_REG_SIZE_DWORD))
		return -EINVAL;

	offset = to_hdm_reg_offset(vdev, offset);
	cur_val = le32_to_cpu(*(u32 *)hdm_reg_virt(vdev, offset));
	if (cur_val & CXL_HDM_DECODER_CTRL_COMMIT_LOCK_BIT)
		return size;

	hdm_decoder_global_cap = le32_to_cpu(*(u32 *)hdm_reg_virt(vdev, 0));
	ro_mask |= CXL_HDM_DECODER_CTRL_DEVICE_BITS_RO;
	rev_mask |= CXL_HDM_DECODER_CTRL_DEVICE_RESERVED;
	if (!(hdm_decoder_global_cap & CXL_HDM_CAP_UIO_SUPPORTED_BIT))
		rev_mask |= CXL_HDM_DECODER_CTRL_UIO_RESERVED;

	new_val &= ~rev_mask;
	cur_val &= ro_mask;
	new_val = (new_val & ~ro_mask) | cur_val;

	if (new_val & CXL_HDM_DECODER_CTRL_COMMIT_BIT)
		new_val |= CXL_HDM_DECODER_CTRL_COMMITTED_BIT;
	else
		new_val &= ~CXL_HDM_DECODER_CTRL_COMMITTED_BIT;

	new_val = cpu_to_le32(new_val);
	memcpy(hdm_reg_virt(vdev, offset), &new_val, size);

	return size;
}

static int setup_mmio_emulation(struct vfio_pci_core_device *vdev)
{
	u64 offset, base;
	int ret;
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	base = hdm_reg_base(vdev);

#define ALLOC_BLOCK(offset, size, read, write) do {			\
		ret = new_mmio_block(vdev, offset, size, read, write);	\
		if (ret)						\
			return ret;					\
	} while (0)

	ALLOC_BLOCK(base + CXL_HDM_DECODER_GLOBAL_CTRL_OFFSET,
		    CXL_REG_SIZE_DWORD,
		    virt_hdm_reg_read,
		    hdm_decoder_global_ctrl_write);

	offset = base + CXL_HDM_DECODER_FIRST_BLOCK_OFFSET;
	while (offset < base + cxl->hdm_reg_size) {
		/* HDM N BASE LOW */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_BASE_LOW_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    hdm_decoder_n_lo_write);

		/* HDM N BASE HIGH */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_BASE_HIGH_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    virt_hdm_reg_write);

		/* HDM N SIZE LOW */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_SIZE_LOW_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    hdm_decoder_n_lo_write);

		/* HDM N SIZE HIGH */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_SIZE_HIGH_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    virt_hdm_reg_write);

		/* HDM N CONTROL */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_CTRL_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    hdm_decoder_n_ctrl_write);

		/* HDM N TARGET LIST LOW */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_TARGET_LIST_LOW_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    virt_hdm_rev_reg_write);

		/* HDM N TARGET LIST HIGH */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_TARGET_LIST_HIGH_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    virt_hdm_rev_reg_write);

		/* HDM N REV */
		ALLOC_BLOCK(offset + CXL_HDM_DECODER_N_REV_OFFSET,
			    CXL_REG_SIZE_DWORD,
			    virt_hdm_reg_read,
			    virt_hdm_rev_reg_write);

		offset += CXL_HDM_DECODER_BLOCK_STRIDE;
	}

#undef ALLOC_BLOCK
	return 0;
}

void vfio_cxl_clean_register_emulation(struct vfio_pci_core_device *vdev)
{
	struct list_head *pos, *n;
	struct vfio_pci_cxl_state *cxl = vdev->cxl;

	list_for_each_safe(pos, n, &cxl->config_regblocks_head)
		kfree(list_entry(pos, struct vfio_emulated_regblock, list));
	list_for_each_safe(pos, n, &cxl->mmio_regblocks_head)
		kfree(list_entry(pos, struct vfio_emulated_regblock, list));
}
EXPORT_SYMBOL_GPL(vfio_cxl_clean_register_emulation);

int vfio_cxl_setup_register_emulation(struct vfio_pci_core_device *vdev)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	int ret;

	INIT_LIST_HEAD(&cxl->config_regblocks_head);
	INIT_LIST_HEAD(&cxl->mmio_regblocks_head);

	ret = setup_mmio_emulation(vdev);
	if (ret)
		goto err;

	return 0;

err:
	vfio_cxl_clean_register_emulation(vdev);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_cxl_setup_register_emulation);

static struct vfio_emulated_regblock *
find_regblock(struct list_head *head, u64 offset, u64 size)
{
	struct vfio_emulated_regblock *block;
	struct list_head *pos;

	list_for_each(pos, head) {
		block = list_entry(pos, struct vfio_emulated_regblock, list);

		if (block->range.start == ALIGN_DOWN(offset,
						     range_len(&block->range)))
			return block;
	}

	return NULL;
}

static ssize_t emulate_read(struct list_head *head,
			    struct vfio_pci_core_device *vdev,
			    char __user *buf, size_t count, loff_t *ppos)
{
	struct vfio_emulated_regblock *block;
	u64 pos = *ppos & VFIO_PCI_OFFSET_MASK;
	ssize_t ret;
	u32 v;

	block = find_regblock(head, pos, count);
	if (!block || !block->read)
		return -ENOTTY;

	if (WARN_ON_ONCE(!IS_ALIGNED(pos, range_len(&block->range))))
		return -EINVAL;

	if (count > range_len(&block->range))
		count = range_len(&block->range);

	ret = block->read(vdev, &v, pos, count);
	if (ret < 0)
		return ret;

	if (copy_to_user(buf, &v, count))
		return -EFAULT;

	return count;
}

static ssize_t emulate_write(struct list_head *head,
			     struct vfio_pci_core_device *vdev,
			     char __user *buf, size_t count, loff_t *ppos)
{
	struct vfio_emulated_regblock *block;
	u64 pos = *ppos & VFIO_PCI_OFFSET_MASK;
	ssize_t ret;
	u32 v;

	block = find_regblock(head, pos, count);
	if (!block || !block->write)
		return -ENOTTY;

	if (WARN_ON_ONCE(!IS_ALIGNED(pos, range_len(&block->range))))
		return -EINVAL;

	if (count > range_len(&block->range))
		count = range_len(&block->range);

	if (copy_from_user(&v, buf, count))
		return -EFAULT;

	ret = block->write(vdev, &v, pos, count);
	if (ret < 0)
		return ret;

	return count;
}

ssize_t vfio_cxl_config_rw(struct vfio_pci_core_device *vdev,
			   char __user *buf, size_t count, loff_t *ppos,
			   bool write)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	loff_t tmp = *ppos;

	if (write)
		return emulate_write(&cxl->config_regblocks_head,
				     vdev, buf, count, &tmp);
	else
		return emulate_read(&cxl->config_regblocks_head,
				    vdev, buf, count, &tmp);
}
EXPORT_SYMBOL_GPL(vfio_cxl_config_rw);

ssize_t vfio_cxl_mmio_bar_rw(struct vfio_pci_core_device *vdev,
			     char __user *buf, size_t count, loff_t *ppos,
			     bool write)
{
	struct vfio_pci_cxl_state *cxl = vdev->cxl;
	size_t done = 0;
	ssize_t ret = 0;
	loff_t tmp, pos = *ppos;

	while (count) {
		tmp = pos;

		if (count >= CXL_REG_SIZE_DWORD &&
		    IS_ALIGNED(pos, CXL_REG_SIZE_DWORD))
			ret = CXL_REG_SIZE_DWORD;
		else if (count >= CXL_REG_SIZE_WORD &&
			 IS_ALIGNED(pos, CXL_REG_SIZE_WORD))
			ret = CXL_REG_SIZE_WORD;
		else
			ret = 1;

		if (write)
			ret = emulate_write(&cxl->mmio_regblocks_head,
					    vdev, buf, ret, &tmp);
		else
			ret = emulate_read(&cxl->mmio_regblocks_head,
					   vdev, buf, ret, &tmp);
		if (ret < 0)
			return ret;

		count -= ret;
		done += ret;
		buf += ret;
		pos += ret;
	}

	*ppos += done;
	return done;
}
EXPORT_SYMBOL_GPL(vfio_cxl_mmio_bar_rw);
