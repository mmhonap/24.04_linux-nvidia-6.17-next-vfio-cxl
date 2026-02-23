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

	INIT_LIST_HEAD(&cxl->config_regblocks_head);
	INIT_LIST_HEAD(&cxl->mmio_regblocks_head);

	return 0;
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
