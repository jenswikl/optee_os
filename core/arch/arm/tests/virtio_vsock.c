// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <initcall.h>
#include <kernel/virtio.h>

static TEE_Result virtio_vsock_echo_init(void)
{
	struct virtio_vsock *vsock = calloc(1, sizeof(*vsock));

	if (!vsock)
		return TEE_ERROR_OUT_OF_MEMORY;

	vsock->cid = 0x8001;
	return virtio_register_vsock(vsock);
}

nex_service_init(virtio_vsock_echo_init);
