/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2025, Linaro Limited
 */
#ifndef __KERNEL_VIRTIO_MSG_FFA_H
#define __KERNEL_VIRTIO_MSG_FFA_H

#include <kernel/thread.h>

void virtio_msg_ffa_recv(struct thread_smc_1_2_regs *args);

#endif /*__KERNEL_VIRTIO_MSG_FFA_H*/
