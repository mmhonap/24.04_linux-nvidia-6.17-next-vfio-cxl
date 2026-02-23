/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VFIO_CXL_PRIV_H
#define VFIO_CXL_PRIV_H

#include <linux/types.h>
#include <linux/vfio_pci_core.h>

int vfio_cxl_setup_register_emulation(struct vfio_pci_core_device *vdev);
void vfio_cxl_clean_register_emulation(struct vfio_pci_core_device *vdev);

#endif /* VFIO_CXL_PRIV_H */
