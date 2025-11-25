// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited
 */

#include <pta_socket.h>
#include <tee_internal_api.h>
#include <tee_isocket.h>
#include <tee_vsocket.h>

#include "tee_socket_private.h"

struct vsock_ctx {
	uint32_t handle;
	uint32_t proto_error;
};

static TEE_Result vsock_open(TEE_iSocketHandle *ctx, void *setup,
			     uint32_t *proto_error)
{
	TEE_vSocket_Setup *vsetup = setup;
	struct vsock_ctx *vctx = NULL;
	TEE_Result res = TEE_SUCCESS;

	if (!ctx || !setup || !proto_error)
		TEE_Panic(0);

	*proto_error = TEE_SUCCESS;

	vctx = TEE_Malloc(sizeof(*vctx), TEE_MALLOC_FILL_ZERO);
	if (!vctx)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = __tee_socket_pta_vsock_open(vsetup, &vctx->handle);
	if (res)
		TEE_Free(vctx);

	*ctx = (TEE_iSocketHandle)vctx;
	return res;
}

static TEE_Result vsock_close(TEE_iSocketHandle ctx)
{
	struct vsock_ctx *vctx = (struct vsock_ctx *)ctx;
	TEE_Result res = TEE_SUCCESS;

	if (ctx == TEE_HANDLE_NULL)
		return TEE_SUCCESS;

	res = __tee_socket_pta_vsock_close(vctx->handle);
	TEE_Free(vctx);

	return res;
}

static TEE_Result vsock_send(TEE_iSocketHandle ctx, const void *buf,
			     uint32_t *length, uint32_t timeout)
{
	struct vsock_ctx *vctx = (struct vsock_ctx *)ctx;
	TEE_Result res = TEE_SUCCESS;
	TEE_vSocket_Send_Flags arg = { };

	if (ctx == TEE_HANDLE_NULL || !buf || !length)
		TEE_Panic(0);

	arg.timeout = timeout;
	arg.buf = (void *)buf;
	arg.buf_len = *length;
	res = __tee_socket_pta_vsock_send_flags(vctx->handle, &arg);
	if (!res)
		*length = arg.buf_len;
	vctx->proto_error = res;

	return res;
}

static TEE_Result vsock_recv(TEE_iSocketHandle ctx, void *buf, uint32_t *length,
			     uint32_t timeout)
{
	struct vsock_ctx *vctx = (struct vsock_ctx *)ctx;
	TEE_Result res = TEE_SUCCESS;

	if (ctx == TEE_HANDLE_NULL || !length || (!buf && *length))
		TEE_Panic(0);

	res = __tee_socket_pta_vsock_recv(vctx->handle, buf, length, timeout);
	vctx->proto_error = res;

	return res;
}

static uint32_t vsock_error(TEE_iSocketHandle ctx)
{
	struct vsock_ctx *vctx = (struct vsock_ctx *)ctx;

	if (ctx == TEE_HANDLE_NULL)
		TEE_Panic(0);

	return vctx->proto_error;
}

static TEE_Result vsock_ioctl(TEE_iSocketHandle ctx, uint32_t commandCode,
			      void *buf, uint32_t *length)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	struct vsock_ctx *vctx = (struct vsock_ctx *)ctx;
	TEE_VSocket_Accept *accept_arg = NULL;
	struct vsock_ctx *vctx2 = NULL;

	if (ctx == TEE_HANDLE_NULL || !length || (!buf && *length))
		TEE_Panic(0);

	if  (__tee_socket_ioctl_cmd_to_proto(commandCode) == 0)
		return TEE_SUCCESS;

	switch (commandCode) {
	case TEE_VSOCK_SEND_FLAGS:
		if (*length != sizeof(TEE_vSocket_Send_Flags)) {
			res = TEE_ERROR_BAD_PARAMETERS;
			break;
		}
		res = __tee_socket_pta_vsock_send_flags(vctx->handle, buf);
		break;
	case TEE_VSOCK_RECV_FLAGS:
		if (*length != sizeof(TEE_vSocket_Recv_Flags)) {
			res = TEE_ERROR_BAD_PARAMETERS;
			break;
		}
		res = __tee_socket_pta_vsock_recv_flags(vctx->handle, buf);
		break;
	case TEE_VSOCK_GET_PEER:
		if (*length != sizeof(TEE_vSocket_Get_Peer)) {
			res = TEE_ERROR_BAD_PARAMETERS;
			break;
		}
		res = __tee_socket_pta_vsock_get_peer(vctx->handle, buf);
		break;
	case TEE_VSOCK_ACCEPT:
		if (*length != sizeof(*accept_arg)) {
			res = TEE_ERROR_BAD_PARAMETERS;
			break;
		}
		accept_arg = buf;
		vctx2 = TEE_Malloc(sizeof(*vctx2), TEE_MALLOC_FILL_ZERO);
		if (!vctx2) {
			res = TEE_ERROR_OUT_OF_MEMORY;
			break;
		}
		res = __tee_socket_pta_vsock_accept(vctx->handle,
						    accept_arg->timeout,
						    &vctx2->handle);
		if (res) {
			TEE_Free(vctx2);
			vctx2 = NULL;
		}
		accept_arg->accept_ctx = vctx2;
		break;
	default:
		TEE_Panic(0);
	}

	vctx->proto_error = res;

	return res;
}

static const TEE_iSocket vsock_instance = {
	.TEE_iSocketVersion = TEE_ISOCKET_VERSION,
	.protocolID = TEE_ISOCKET_PROTOCOLID_VSOCK,
	.open = vsock_open,
	.close = vsock_close,
	.send = vsock_send,
	.recv = vsock_recv,
	.error = vsock_error,
	.ioctl = vsock_ioctl,
};

TEE_iSocket *const TEE_vSocket = &vsock_instance;
