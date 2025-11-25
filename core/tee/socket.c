// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2016-2025, Linaro Limited
 */

#include <assert.h>
#include <kernel/pseudo_ta.h>
#include <kernel/user_access.h>
#include <kernel/virtio_vsock.h>
#include <mm/mobj.h>
#include <optee_rpc_cmd.h>
#include <pta_socket.h>
#include <string.h>
#include <tee/tee_fs_rpc.h>
#include <tee_vsocket.h>

static uint32_t get_instance_id(struct ts_session *sess)
{
	return sess->ctx->ops->get_instance_id(sess->ctx);
}

static TEE_Result socket_open(uint32_t instance_id, uint32_t param_types,
			      TEE_Param params[TEE_NUM_PARAMS])
{
	struct thread_param tpm[4] = { };
	struct mobj *mobj = NULL;
	TEE_Result res = TEE_SUCCESS;
	void *va = NULL;
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_INPUT,
					  TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT);

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	va = thread_rpc_shm_cache_alloc(THREAD_SHM_CACHE_USER_SOCKET,
					THREAD_SHM_TYPE_APPLICATION,
					params[1].memref.size, &mobj);
	if (!va)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = copy_from_user(va, params[1].memref.buffer,
			     params[1].memref.size);
	if (res)
		return res;

	tpm[0] = THREAD_PARAM_VALUE(IN, OPTEE_RPC_SOCKET_OPEN, instance_id, 0);
	tpm[1] = THREAD_PARAM_VALUE(IN,
				    params[0].value.b, /* server port number */
				    params[2].value.a, /* protocol */
				    params[0].value.a  /* ip version */);
	tpm[2] = THREAD_PARAM_MEMREF(IN, mobj, 0, params[1].memref.size);
	tpm[3] = THREAD_PARAM_VALUE(OUT, 0, 0, 0);

	res = thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 4, tpm);
	if (res == TEE_SUCCESS)
		params[3].value.a = tpm[3].u.value.a;

	return res;
}

static TEE_Result socket_close(uint32_t instance_id, uint32_t param_types,
			       TEE_Param params[TEE_NUM_PARAMS])
{
	struct thread_param tpm = { };
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	tpm = THREAD_PARAM_VALUE(IN, OPTEE_RPC_SOCKET_CLOSE, instance_id,
				 params[0].value.a);

	return thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 1, &tpm);
}

static TEE_Result socket_send(uint32_t instance_id, uint32_t param_types,
			      TEE_Param params[TEE_NUM_PARAMS])
{
	struct thread_param tpm[3] = { };
	struct mobj *mobj = NULL;
	TEE_Result res = TEE_SUCCESS;
	void *va = NULL;
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_NONE);

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	va = thread_rpc_shm_cache_alloc(THREAD_SHM_CACHE_USER_SOCKET,
					THREAD_SHM_TYPE_APPLICATION,
					params[1].memref.size, &mobj);
	if (!va)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = copy_from_user(va, params[1].memref.buffer,
			     params[1].memref.size);
	if (res)
		return res;

	tpm[0] = THREAD_PARAM_VALUE(IN, OPTEE_RPC_SOCKET_SEND, instance_id,
				    params[0].value.a /* handle */);
	tpm[1] = THREAD_PARAM_MEMREF(IN, mobj, 0, params[1].memref.size);
	tpm[2] = THREAD_PARAM_VALUE(INOUT, params[0].value.b, /* timeout */
				     0, 0);

	res = thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 3, tpm);
	params[2].value.a = tpm[2].u.value.b; /* transmitted bytes */

	return res;
}

static TEE_Result socket_recv(uint32_t instance_id, uint32_t param_types,
			      TEE_Param params[TEE_NUM_PARAMS])
{
	struct thread_param tpm[3] = { };
	struct mobj *mobj = NULL;
	TEE_Result res = TEE_SUCCESS;
	void *va = NULL;
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_OUTPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	if (params[1].memref.size) {
		va = thread_rpc_shm_cache_alloc(THREAD_SHM_CACHE_USER_SOCKET,
						THREAD_SHM_TYPE_APPLICATION,
						params[1].memref.size, &mobj);
		if (!va)
			return TEE_ERROR_OUT_OF_MEMORY;
	}

	tpm[0] = THREAD_PARAM_VALUE(IN, OPTEE_RPC_SOCKET_RECV, instance_id,
				    params[0].value.a /* handle */);
	tpm[1] = THREAD_PARAM_MEMREF(OUT, mobj, 0, params[1].memref.size);
	tpm[2] = THREAD_PARAM_VALUE(IN, params[0].value.b /* timeout */, 0, 0);

	res = thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 3, tpm);

	if (params[1].memref.size) {
		TEE_Result res2 = TEE_SUCCESS;

		res2 = copy_to_user(params[1].memref.buffer, va,
				    MIN(params[1].memref.size,
					tpm[1].u.memref.size));
		if (res2)
			return res2;
	}
	params[1].memref.size = tpm[1].u.memref.size;

	return res;
}

static TEE_Result socket_ioctl(uint32_t instance_id, uint32_t param_types,
			       TEE_Param params[TEE_NUM_PARAMS])
{
	struct thread_param tpm[3] = { };
	struct mobj *mobj = NULL;
	TEE_Result res = TEE_SUCCESS;
	void *va = NULL;
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_INOUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	va = thread_rpc_shm_cache_alloc(THREAD_SHM_CACHE_USER_SOCKET,
					THREAD_SHM_TYPE_APPLICATION,
					params[1].memref.size, &mobj);
	if (!va)
		return TEE_ERROR_OUT_OF_MEMORY;

	res = copy_from_user(va, params[1].memref.buffer,
			     params[1].memref.size);
	if (res)
		return res;

	tpm[0] = THREAD_PARAM_VALUE(IN, OPTEE_RPC_SOCKET_IOCTL, instance_id,
				    params[0].value.a /* handle */);
	tpm[1] = THREAD_PARAM_MEMREF(INOUT, mobj, 0, params[1].memref.size);
	tpm[2] = THREAD_PARAM_VALUE(IN, params[0].value.b /* ioctl command */,
				    0, 0);

	res = thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 3, tpm);
	if (tpm[1].u.memref.size <= params[1].memref.size) {
		TEE_Result res2 = TEE_SUCCESS;

		res2 = copy_to_user(params[1].memref.buffer, va,
				    tpm[1].u.memref.size);
		if (res2)
			return res2;
	}

	params[1].memref.size = tpm[1].u.memref.size;

	return res;
}

#ifdef CFG_VIRTIO_VSOCK
static struct handle_db *get_vsock_hdb(void)
{
	return &to_user_ta_ctx(ts_get_calling_session()->ctx)->vsock_hdb;
}

static TEE_Result socket_vsock_open(uint32_t instance_id __unused,
				    uint32_t param_types,
				    TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	uint32_t type = 0;
	int h = 0;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	switch (params[0].value.a) {
	case TEE_VSOCKET_TYPE_STREAM:
		type = VIRTIO_VSOCK_TYPE_STREAM;
		break;
	case TEE_VSOCKET_TYPE_SEQPACKET:
		type = VIRTIO_VSOCK_TYPE_SEQPACKET;
		break;
	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}

	res = virtio_vsock_listen(params[0].value.b, type, hdb, &vvs);
	if (res)
		return res;

	h = handle_get(hdb, vvs);
	if (h < 0) {
		virtio_vsock_close(vvs);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	params[1].value.a = h;

	return TEE_SUCCESS;
}

static TEE_Result socket_vsock_close(uint32_t instance_id __unused,
				     uint32_t param_types,
				     TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_put(hdb, params[0].value.a);
	if (!vvs)
		return TEE_ERROR_BAD_PARAMETERS;

	virtio_vsock_close(vvs);

	return TEE_SUCCESS;
}

static void vsock_recv(uint8_t *buf, size_t *blen, uint32_t *flags,
		       struct virtio_vsock_socket *vvs)
{
	struct virtio_vsock_msg *m = NULL;
	size_t dst_offs = 0;
	uint32_t f = 0;
	size_t l = 0;

	m = virtio_vsock_msgq_peek(vvs, VIRTIO_VSOCKET_MSG_TYPE_DATA, 0);
	while (m && dst_offs < *blen) {
		l = MIN(*blen - dst_offs, m->data.len - m->data.offs);
		memcpy(buf + dst_offs, (uint8_t *)m->data.buf + m->data.offs,
		       l);
		dst_offs += l;
		m->data.offs += l;
		/*
		 * The destination buffer is full if there are data left in
		 * this message.
		 */
		if (m->data.offs < m->data.len)
			break;

		if (m->data.flags & VIRTIO_VSOCK_SEQ_EOM)
			f |= TEE_VSOCK_FLAG_SEQ_EOM;
		if (m->data.flags & VIRTIO_VSOCK_SEQ_EOR)
			f |= TEE_VSOCK_FLAG_SEQ_EOR;

		free(m->data.buf);
		virtio_vsock_msgq_dequeue(vvs, m);

		if (f && flags)
			break;

		m = virtio_vsock_msgq_peek(vvs, VIRTIO_VSOCKET_MSG_TYPE_DATA,
					   0);
	}

	if (flags)
		*flags = f;
	*blen = dst_offs;
}

static TEE_Result socket_vsock_recv(uint32_t instance_id __unused,
				    uint32_t param_types,
				    TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_OUTPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;
	struct virtio_vsock_msg *m = NULL;
	TEE_Result res = TEE_SUCCESS;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_lookup(hdb, params[0].value.a);
	if (!vvs || vvs->type != VIRTIO_VSOCK_TYPE_STREAM)
		return TEE_ERROR_BAD_PARAMETERS;

	virtio_vsock_msgq_lock(vvs);
	m = virtio_vsock_msgq_peek(vvs, VIRTIO_VSOCKET_MSG_TYPE_DATA,
				   params[0].value.b);
	if (!m) {
		params[1].memref.size = 0;
		goto out;
	}

	vsock_recv(params[1].memref.buffer, &params[1].memref.size, NULL, vvs);
out:
	virtio_vsock_msgq_unlock(vvs);

	return res;
}

static TEE_Result socket_vsock_recv_flags(uint32_t instance_id __unused,
					  uint32_t param_types,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_OUTPUT,
					  TEE_PARAM_TYPE_VALUE_INOUT,
					  TEE_PARAM_TYPE_VALUE_INOUT);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;
	struct virtio_vsock_msg *m = NULL;
	TEE_Result res = TEE_SUCCESS;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_lookup(hdb, params[0].value.a);
	if (!vvs)
		return TEE_ERROR_BAD_PARAMETERS;

	virtio_vsock_msgq_lock(vvs);
	m = virtio_vsock_msgq_peek(vvs, VIRTIO_VSOCKET_MSG_TYPE_DATA,
				   params[0].value.b);
	if (!m) {
		params[1].memref.size = 0;
		params[2].value.a = 0;
		goto out;
	}
	if (vvs->type == VIRTIO_VSOCK_TYPE_STREAM) {
		vsock_recv(params[1].memref.buffer,
			   &params[1].memref.size, NULL, vvs);
	} else {
		assert(vvs->type == TEE_VSOCKET_TYPE_SEQPACKET);
		vsock_recv(params[1].memref.buffer, &params[1].memref.size,
			   &params[2].value.b, vvs);
	}
out:
	virtio_vsock_msgq_unlock(vvs);

	return res;
}

static TEE_Result socket_vsock_send_flags(uint32_t instance_id __unused,
					  uint32_t param_types,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_MEMREF_INPUT,
					  TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	size_t sz = 0;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_lookup(hdb, params[0].value.a);
	if (!vvs)
		return TEE_ERROR_BAD_PARAMETERS;

	sz = params[1].memref.size;
	res = virtio_vsock_send(vvs, params[1].memref.buffer, &sz,
				params[2].value.a, params[0].value.b);
	if (!res)
		params[3].value.a = sz;

	return res;
}

static TEE_Result socket_vsock_get_peer(uint32_t instance_id __unused,
					uint32_t param_types,
					TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_NONE);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_lookup(hdb, params[0].value.a);
	if (!vvs || vvs->listen)
		return TEE_ERROR_BAD_PARAMETERS;

	params[1].value.a = high32_from_64(vvs->dst_cid);
	params[1].value.b = low32_from_64(vvs->dst_cid);
	params[1].value.a = vvs->dst_port;

	return TEE_SUCCESS;
}

static TEE_Result socket_vsock_accept(uint32_t instance_id __unused,
				      uint32_t param_types,
				      TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct handle_db *hdb = get_vsock_hdb();
	struct virtio_vsock_socket *vvs = NULL;
	struct virtio_vsock_msg *m = NULL;
	TEE_Result res = TEE_SUCCESS;
	int h = 0;

	if (exp_pt != param_types) {
		DMSG("got param_types 0x%x, expected 0x%x",
		     param_types, exp_pt);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	vvs = handle_lookup(hdb, params[0].value.a);
	if (!vvs || !vvs->listen)
		return TEE_ERROR_BAD_PARAMETERS;

	virtio_vsock_msgq_lock(vvs);
	m = virtio_vsock_msgq_peek(vvs, VIRTIO_VSOCKET_MSG_TYPE_REQ,
				   params[0].value.b);
	if (!m) {
		res = TEE_ERROR_TIMEOUT;
		goto out;
	}
	assert(m->type == VIRTIO_VSOCKET_MSG_TYPE_REQ);
	h = handle_get(hdb, m->vvs_req);
	if (h < 0) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	params[1].value.a = h;
	virtio_vsock_msgq_dequeue(vvs, m);
out:
	virtio_vsock_msgq_unlock(vvs);

	return res;
}
#endif /*CFG_VIRTIO_VSOCK*/

typedef TEE_Result (*ta_func)(uint32_t instance_id, uint32_t param_types,
			      TEE_Param params[TEE_NUM_PARAMS]);

static const ta_func ta_funcs[] = {
	[PTA_SOCKET_OPEN] = socket_open,
	[PTA_SOCKET_CLOSE] = socket_close,
	[PTA_SOCKET_SEND] = socket_send,
	[PTA_SOCKET_RECV] = socket_recv,
	[PTA_SOCKET_IOCTL] = socket_ioctl,
#ifdef CFG_VIRTIO_VSOCK
	[PTA_SOCKET_VSOCK_OPEN] = socket_vsock_open,
	[PTA_SOCKET_VSOCK_CLOSE] = socket_vsock_close,
	[PTA_SOCKET_VSOCK_RECV] = socket_vsock_recv,
	[PTA_SOCKET_VSOCK_RECV_FLAGS] = socket_vsock_recv_flags,
	[PTA_SOCKET_VSOCK_SEND_FLAGS] = socket_vsock_send_flags,
	[PTA_SOCKET_VSOCK_GET_PEER] = socket_vsock_get_peer,
	[PTA_SOCKET_VSOCK_ACCEPT] = socket_vsock_accept,
#endif
};

/*
 * Trusted Application Entry Points
 */

static TEE_Result pta_socket_open_session(uint32_t param_types __unused,
			TEE_Param pParams[TEE_NUM_PARAMS] __unused,
			void **sess_ctx)
{
	struct ts_session *s = ts_get_calling_session();

	/* Check that we're called from a TA */
	if (!s || !is_user_ta_ctx(s->ctx))
		return TEE_ERROR_ACCESS_DENIED;

	*sess_ctx = (void *)(vaddr_t)get_instance_id(s);

	return TEE_SUCCESS;
}

static void pta_socket_close_session(void *sess_ctx)
{
	TEE_Result res;
	struct thread_param tpm = {
		.attr = THREAD_PARAM_ATTR_VALUE_IN, .u.value = {
			.a = OPTEE_RPC_SOCKET_CLOSE_ALL, .b = (vaddr_t)sess_ctx,
		},
	};

	res = thread_rpc_cmd(OPTEE_RPC_CMD_SOCKET, 1, &tpm);
	if (res != TEE_SUCCESS)
		DMSG("OPTEE_RPC_SOCKET_CLOSE_ALL failed: %#" PRIx32, res);
}

static TEE_Result pta_socket_invoke_command(void *sess_ctx, uint32_t cmd_id,
			uint32_t param_types, TEE_Param params[TEE_NUM_PARAMS])
{
	if (cmd_id < ARRAY_SIZE(ta_funcs) && ta_funcs[cmd_id])
		return ta_funcs[cmd_id]((vaddr_t)sess_ctx, param_types, params);

	return TEE_ERROR_NOT_IMPLEMENTED;
}

pseudo_ta_register(.uuid = PTA_SOCKET_UUID, .name = "socket",
		   .flags = PTA_DEFAULT_FLAGS | TA_FLAG_CONCURRENT,
		   .open_session_entry_point = pta_socket_open_session,
		   .close_session_entry_point = pta_socket_close_session,
		   .invoke_command_entry_point = pta_socket_invoke_command);
