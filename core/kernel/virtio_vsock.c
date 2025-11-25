// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <bitstring.h>
#include <confine_array_index.h>
#include <initcall.h>
#include <kernel/delay.h>
#include <kernel/virtio.h>
#include <kernel/virtio_vsock.h>
#include <mm/core_mmu.h>
#include <string.h>

#define VIRTIO_VSOCK_F_STREAM			0
#define VIRTIO_VSOCK_F_SEQPACKET		1
#define VIRTIO_VSOCK_F_NO_IMPLIED_STREAM	2

TAILQ_HEAD(virtio_vsock_socket_head, virtio_vsock_socket);

struct virtio_vsock_device {
	struct virtio_device vdev;
	uint64_t cid;
};

struct virtio_vsock_hdr {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t len;
	uint16_t type;
	uint16_t op;
	uint32_t flags;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} __packed;

enum virtio_vsock_vq_idx {
	VIRTIO_VSOCK_VQ_IDX_RX = 0,
	VIRTIO_VSOCK_VQ_IDX_TX,
	VIRTIO_VSOCK_VQ_IDX_EVENT,
	VIRTIO_VSOCK_VQ_IDX_COUNT,
};

#define VIRTIO_VSOCK_DEVICE_ID		19

#define VIRTIO_VSOCK_OP_INVALID		0
/* Connect operations */
#define VIRTIO_VSOCK_OP_REQUEST		1
#define VIRTIO_VSOCK_OP_RESPONSE	2
#define VIRTIO_VSOCK_OP_RST		3
#define VIRTIO_VSOCK_OP_SHUTDOWN	4
/* To send payload */
#define VIRTIO_VSOCK_OP_RW		5
/* Tell the peer our credit info */
#define VIRTIO_VSOCK_OP_CREDIT_UPDATE	6
/* Request the peer to send the credit info to us */
#define VIRTIO_VSOCK_OP_CREDIT_REQUEST	7

#define VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE	0
#define VIRTIO_VSOCK_SHUTDOWN_F_SEND	1

#define VIRTIO_VSOCK_CID_HOST		2

static const struct virtio_description vsock_desc;
static const uint8_t vsock_features[] = {
	VIRTIO_F_VERSION_1,
	VIRTIO_VSOCK_F_STREAM,
	VIRTIO_VSOCK_F_SEQPACKET,
	VIRTIO_VSOCK_F_NO_IMPLIED_STREAM,
	VIRTIO_F_ACCESS_PLATFORM,
};

static struct mutex sock_lock = MUTEX_INITIALIZER;
static size_t sock_vvs_msgs_wait_count;
static struct condvar sock_vvs_msgs_cv = CONDVAR_INITIALIZER;
static size_t sock_vvs_send_wait_count;
static struct condvar sock_vvs_send_cv = CONDVAR_INITIALIZER;
static struct virtio_vsock_socket_head sockets_head =
	TAILQ_HEAD_INITIALIZER(sockets_head);
static struct virtio_vsock_socket_head sockets_backlog_head =
	TAILQ_HEAD_INITIALIZER(sockets_backlog_head);

static struct virtio_vsock_socket *find_listening_vsock(uint32_t port,
							uint16_t type)
{
	struct virtio_vsock_socket *v = NULL;

	TAILQ_FOREACH(v, &sockets_head, link)
		if (v->listen && v->type == type && v->src_port == port)
			return v;

	return NULL;
}

static bool cmp_socket(const struct virtio_vsock_socket *vvs1,
		       const struct virtio_vsock_socket *vvs2)
{
	return vvs1->src_cid == vvs2->src_cid &&
	       vvs1->dst_cid == vvs2->dst_cid &&
	       vvs1->src_port == vvs2->src_port &&
	       vvs1->dst_port == vvs2->dst_port &&
	       vvs1->type == vvs2->type;
}

static struct virtio_vsock_socket *
find_socket(const struct virtio_vsock_socket *vvs)
{
	struct virtio_vsock_socket *v = NULL;

	TAILQ_FOREACH(v, &sockets_head, link)
		if (cmp_socket(v, vvs))
			return v;

	return NULL;
}

static struct virtio_vsock_socket *find_socket2(uint64_t src_cid,
						uint64_t dst_cid,
						uint32_t src_port,
						uint32_t dst_port,
						uint16_t type)
{
	struct virtio_vsock_socket vvs = {
		.src_cid = src_cid,
		.dst_cid = dst_cid,
		.src_port = src_port,
		.dst_port = dst_port,
		.type = type,
	};

	return find_socket(&vvs);
}

static void add_msg(struct virtio_vsock_msg_head *msgs,
		    struct virtio_vsock_msg *m)
{
	TAILQ_INSERT_TAIL(msgs, m, link);
	if (sock_vvs_msgs_wait_count)
		condvar_broadcast(&sock_vvs_msgs_cv);
}

static TEE_Result add_socket(struct virtio_vsock_socket *vvs,
			     struct virtio_vsock_msg_head *msgs)
{
	struct virtio_vsock_msg *msg = NULL;

	if (msgs) {
		msg = calloc(1, sizeof(*msg));
		if (!msg)
			return TEE_ERROR_OUT_OF_MEMORY;
		msg->type = VIRTIO_VSOCKET_MSG_TYPE_REQ;
		msg->vvs_req = vvs;
	}

	if (find_socket(vvs)) {
		free(msg);
		return TEE_ERROR_ACCESS_CONFLICT;
	}

	TAILQ_INSERT_TAIL(&sockets_head, vvs, link);
	if (msg)
		add_msg(msgs, msg);

	return TEE_SUCCESS;
}

static struct virtio_vsock_device *vdev_to_vsdev(struct virtio_device *vdev)
{
	assert(vdev->desc == &vsock_desc);

	return container_of(vdev, struct virtio_vsock_device, vdev);
}

static void init_resp_hdr(struct virtio_vsock_hdr *resp,
			  const struct virtio_vsock_hdr *req, uint16_t op)
{
	*resp = (struct virtio_vsock_hdr){
		.src_cid = req->dst_cid,
		.dst_cid = req->src_cid,
		.src_port = req->dst_port,
		.dst_port = req->src_port,
		.type = req->type,
		.op = op,
	};
}

static void handle_op_request(struct virtq_read_ctx *vqr,
			      struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_device *dev = vdev_to_vsdev(vqr->vq->vdev);
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_socket *lvvs = NULL;
	struct virtio_vsock_socket *vvs = NULL;
	struct virtio_vsock_hdr resp = { };

	init_resp_hdr(&resp, req, VIRTIO_VSOCK_OP_RESPONSE);
	if (req->src_cid != dev->cid || !req->src_port) {
		DMSG("Bad req");
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}

	lvvs = find_listening_vsock(req->dst_port, req->type);
	if (!lvvs || lvvs->src_cid != req->dst_cid) {
		DMSG("No listener");
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}

	vvs = calloc(1, sizeof(*vvs));
	if (!vvs) {
		DMSG("Calloc failed");
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}
	vvs->src_cid = req->src_cid;
	vvs->dst_cid = req->dst_cid;
	vvs->src_port = req->src_port;
	vvs->dst_port = req->dst_port;
	vvs->type = req->type;
	vvs->owner = lvvs->owner;
	vvs->dev = dev;
	vvs->buf_alloc = SMALL_PAGE_SIZE;
	vvs->peer_buf_alloc = req->buf_alloc;
	vvs->peer_fwd_cnt = req->fwd_cnt;
	TAILQ_INIT(&vvs->msgs);
	if (add_socket(vvs, &lvvs->msgs)) {
		DMSG("Connection already exists");
		free(vvs);
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}

	resp.buf_alloc = vvs->buf_alloc;
out:
	res = virtq_write_start(vqr->vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		return;
	}

	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		return;
	}

	virtq_write_finish(&wbuf, sizeof(resp));
}

static void handle_op_rw(struct virtq_read_ctx *vqr,
			 struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_msg *msg = NULL;
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = { };

	vvs = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port, req->type);
	if (!vvs) {
		DMSG("Connection not found");
		goto err;
	}

	vvs->peer_buf_alloc = req->buf_alloc;
	vvs->peer_fwd_cnt = req->fwd_cnt;

	if (vvs->rx_cnt - vvs->fwd_cnt > vvs->buf_alloc) {
		DMSG("Peer is overdrafting from allocated buffer");
		goto err;
	}

	msg = calloc(1, sizeof(*msg));
	if (!msg) {
		DMSG("calloc");
		return;
	}
	msg->type = VIRTIO_VSOCKET_MSG_TYPE_DATA;
	msg->data.buf = calloc(1, req->len);
	if (!msg->data.buf) {
		free(msg);
		DMSG("calloc");
		return;
	}

	msg->data.len = req->len;
	msg->data.flags = req->flags & (VIRTIO_VSOCK_SEQ_EOM |
					VIRTIO_VSOCK_SEQ_EOR);

	res = virtq_read_copy(msg->data.buf, vqr, sizeof(*req), req->len);
	if (res) {
		DMSG("virtq_read_copy: res %#"PRIx32, res);
		goto err;
	}
	if (TRACE_LEVEL >= TRACE_FLOW)
		DHEXDUMP(msg->data.buf, msg->data.len);

	vvs->fwd_cnt += req->len;
	virtio_vsock_msgq_lock(vvs);
	add_msg(&vvs->msgs, msg);
	virtio_vsock_msgq_unlock(vvs);
	return;

err:
	init_resp_hdr(&resp, req, VIRTIO_VSOCK_OP_RST);
	res = virtq_write_start(vqr->vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		return;
	}

	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		return;
	}

	virtq_write_finish(&wbuf, sizeof(resp));
}

static void handle_op_shutdown(struct virtq_read_ctx *vqr,
			       struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = { };

	init_resp_hdr(&resp, req, VIRTIO_VSOCK_OP_SHUTDOWN);
	resp.flags = BIT(VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE) |
		     BIT(VIRTIO_VSOCK_SHUTDOWN_F_SEND);
	res = virtq_write_start(vqr->vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		return;
	}

	vvs = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port, req->type);
	if (!vvs) {
		DMSG("Connection not found");
		resp.op = VIRTIO_VSOCK_OP_RST;
		resp.flags = 0;
		goto out;
	}
	vvs->dead = true;

out:
	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		return;
	}

	virtq_write_finish(&wbuf, sizeof(resp));
}

static void handle_op_credit_update(struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_socket *vvs = NULL;

	vvs = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port, req->type);
	if (vvs) {
		vvs->peer_buf_alloc = req->buf_alloc;
		vvs->peer_fwd_cnt = req->fwd_cnt;
	}
}

static void handle_op_credit_request(struct virtq_read_ctx *vqr,
				     struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = { };

	init_resp_hdr(&resp, req, VIRTIO_VSOCK_OP_CREDIT_UPDATE);
	res = virtq_write_start(vqr->vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		return;
	}

	vvs = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port, req->type);
	if (!vvs) {
		DMSG("Connection not found");
		resp.op = VIRTIO_VSOCK_OP_RST;
		resp.flags = 0;
		goto out;
	}

	vvs->peer_buf_alloc = req->buf_alloc;
	vvs->peer_fwd_cnt = req->fwd_cnt;
	resp.buf_alloc = vvs->buf_alloc;
	resp.fwd_cnt = vvs->fwd_cnt;

out:
	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		return;
	}

	virtq_write_finish(&wbuf, sizeof(resp));
}

static void handle_op_rst(struct virtq_read_ctx *vqr,
			  struct virtio_vsock_hdr *req)
{
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = { };

	init_resp_hdr(&resp, req, VIRTIO_VSOCK_OP_RST);
	res = virtq_write_start(vqr->vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		return;
	}

	vvs = find_socket2(req->dst_cid, req->src_cid, req->dst_port,
			   req->src_port, req->type);
	if (vvs) {
		DMSG("Killing socket");
		vvs->dead = true;
	}

	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		return;
	}

	virtq_write_finish(&wbuf, sizeof(resp));
}

static void handle_rx_payload(struct virtq_read_ctx *vqr)
{
	struct virtio_vsock_hdr hdr = { };
	TEE_Result res = TEE_SUCCESS;

	res = virtq_read_copy(&hdr, vqr, 0, sizeof(hdr));
	if (res) {
		DMSG("virtq_read_copy: res %#"PRIx32, res);
		return;
	}
	FMSG("src_cid %#"PRIx64" dst_cid %#"PRIx64,
	     hdr.src_cid, hdr.dst_cid);
	FMSG("src_port %#"PRIx32" dst_port %#"PRIx32,
	     hdr.src_port, hdr.dst_port);
	FMSG("len %"PRIu32" type %"PRIu16" op %"PRIu16,
	     hdr.len, hdr.type, hdr.op);
	FMSG("flags %#"PRIx32" buf_alloc %"PRIu32" fwd_cnt %"PRIu32,
	     hdr.flags, hdr.buf_alloc, hdr.fwd_cnt);

	switch (hdr.op) {
	case VIRTIO_VSOCK_OP_REQUEST:
		DMSG("VIRTIO_VSOCK_OP_REQUEST");
		handle_op_request(vqr, &hdr);
		break;
	case VIRTIO_VSOCK_OP_RW:
		DMSG("VIRTIO_VSOCK_OP_RW");
		handle_op_rw(vqr, &hdr);
		break;
	case VIRTIO_VSOCK_OP_RST:
		DMSG("VIRTIO_VSOCK_OP_RST");
		handle_op_rst(vqr, &hdr);
		break;
	case VIRTIO_VSOCK_OP_SHUTDOWN:
		DMSG("VIRTIO_VSOCK_OP_SHUTDOWN");
		handle_op_shutdown(vqr, &hdr);
		break;
	case VIRTIO_VSOCK_OP_CREDIT_UPDATE:
		DMSG("VIRTIO_VSOCK_OP_CREDIT_UPDATE");
		handle_op_credit_update(&hdr);
		break;
	case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
		DMSG("VIRTIO_VSOCK_OP_CREDIT_REQUEST");
		handle_op_credit_request(vqr, &hdr);
		break;
	default:
		DMSG("Unknown op %"PRIu16, hdr.op);
	}
}

static void vsock_vq_host_rx_callback(struct virtq *vq)
{
	struct virtq_read_ctx vqr = { };

	DMSG("idx %u", vq->vq_id);

	while (!virtq_read_start(vq, &vqr)) {
		handle_rx_payload(&vqr);
		virtq_read_finish(&vqr);
	}
}

static bool reply_op(struct virtq *vq, struct virtio_vsock_socket *vvs,
		     uint16_t op)
{
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = {
		.src_cid = vvs->src_cid,
		.dst_cid = vvs->dst_cid,
		.src_port = vvs->src_port,
		.dst_port = vvs->dst_port,
		.type = vvs->type,
		.op = op,
	};

	if (virtq_write_start(vq, &wbuf, sizeof(resp)))
		return false;
	if (virtq_write_copy(&resp, &wbuf, 0, sizeof(resp)))
		return false;
	virtq_write_finish(&wbuf, sizeof(resp));

	return true;
}

static bool process_backlog(struct virtq *vq)
{
	struct virtio_vsock_socket *vvs = NULL;

	assert(vq == vq->vdev->vqs + VIRTIO_VSOCK_VQ_IDX_RX);

	while (!TAILQ_EMPTY(&sockets_backlog_head)) {
		vvs = TAILQ_FIRST(&sockets_backlog_head);
		assert((vvs->pending_op == VIRTIO_VSOCK_OP_RST &&
			vvs->dead && !vvs->owner) ||
		       vvs->pending_op == VIRTIO_VSOCK_OP_RESPONSE ||
		       vvs->pending_op == VIRTIO_VSOCK_OP_SHUTDOWN);
		if (!reply_op(vq, vvs, vvs->pending_op))
			break;

		TAILQ_REMOVE(&sockets_backlog_head, vvs, link_backlog);
		/*
		 * The if the socket is fully dead, then the only freeing
		 * it remains to do.
		 */
		if (vvs->pending_op == VIRTIO_VSOCK_OP_RST)
			free(vvs);
		else
			vvs->pending_op = VIRTIO_VSOCK_OP_INVALID;
	}

	return TAILQ_EMPTY(&sockets_backlog_head);
}

static void vsock_vq_host_tx_callback(struct virtq *vq)
{
	DMSG("idx %u", vq->vq_id);
	mutex_lock(&sock_lock);
	process_backlog(vq);
	mutex_unlock(&sock_lock);
}

static void vsock_vq_event_callback(struct virtq *vq)
{
	DMSG("idx %u", vq->vq_id);
}

static void vsock_get_features(struct virtio_device *vdev __unused,
			       bitstr_t *bs, size_t count, size_t offset)
{
	size_t n = 0;

	assert(count + offset <= VIRTIO_MAX_FEATURE_BIT_COUNT);

	for (n = 0; n < ARRAY_SIZE(vsock_features); n++) {
		if (vsock_features[n] >= offset &&
		    vsock_features[n] - offset < count)
			bit_set(bs, vsock_features[n] - offset);
	}
}

static bool have_feature(uint8_t f)
{
	size_t n = 0;

	for (n = 0; n < ARRAY_SIZE(vsock_features); n++)
		if (f == vsock_features[n])
			return true;

	return false;
}

static void vsock_set_features(struct virtio_device *vdev, bitstr_t *bs,
			       size_t count, size_t offset)
{
	size_t n = 0;

	assert(count + offset <= VIRTIO_MAX_FEATURE_BIT_COUNT);

	for (n = 0; n < count; n++) {
		if (!bit_test(bs, n))
			continue;
		if (!have_feature(n + offset)) {
			EMSG("Unknown feature %zu", n + offset);
			vdev->features_ok = false;
			return;
		}
		bit_set(vdev->features, n + offset);
	}

	vdev->features_ok = true;
}

static void vsock_get_config(struct virtio_device *vdev, void *data,
			     size_t count, size_t offset)
{
	struct virtio_vsock_device *dev = vdev_to_vsdev(vdev);
	uint8_t *vsock_config = (uint8_t *)&dev->cid;

	assert(sizeof(dev->cid) == vsock_desc.config_size);
	assert(count + offset <= sizeof(dev->cid));

	memcpy(data, vsock_config + offset, count);
}

static void vsock_set_virtque(struct virtio_device *vdev, struct virtq *vq)
{
	if (!vq->vdev) {
		vq->vdev = vdev;
		switch (vq->vq_id) {
		case VIRTIO_VSOCK_VQ_IDX_RX:
			vq->callback = vsock_vq_host_tx_callback;
			break;
		case VIRTIO_VSOCK_VQ_IDX_TX:
			vq->callback = vsock_vq_host_rx_callback;
			break;
		default:
			vq->callback = vsock_vq_event_callback;
			break;
		}
		if (virtq_enable(vq)) {
			vq->vdev = NULL;
			vq->callback = NULL;
		}
	}
}

static struct virtq *vsock_get_virtque(struct virtio_device *vdev,
				       size_t vq_idx)
{
	if (vq_idx >= vdev->desc->vq_count)
		return NULL;

	/*
	 * Functions calling this function will dereference the returned
	 * pointer so it might leak data from speculative execution. We'd
	 * rather abstract the handling speculation issues than require all
	 * callers to deal with it themselves.
	 */

	return vdev->vqs + confine_array_index(vq_idx, vdev->desc->vq_count);
}

TEE_Result virtio_vsock_listen(uint32_t port, uint16_t type, void *owner,
			       struct virtio_vsock_socket **vvs_ret)
{
	struct virtio_vsock_socket *vvs = NULL;
	TEE_Result res = TEE_SUCCESS;

	vvs = calloc(1, sizeof(*vvs));
	if (!vvs)
		return TEE_ERROR_OUT_OF_MEMORY;
	vvs->src_port = port;
	vvs->src_cid = VIRTIO_VSOCK_CID_HOST;
	vvs->type = type;
	vvs->owner = owner;
	vvs->listen = true;
	vvs->buf_alloc = SMALL_PAGE_SIZE;
	TAILQ_INIT(&vvs->msgs);

	mutex_lock(&sock_lock);
	if (find_listening_vsock(port, type)) {
		res = TEE_ERROR_ACCESS_CONFLICT;
		goto out;
	}
	TAILQ_INSERT_TAIL(&sockets_head, vvs, link);
out:
	mutex_unlock(&sock_lock);
	if (res)
		free(vvs);
	else
		*vvs_ret = vvs;

	return res;
}

void virtio_vsock_close(struct virtio_vsock_socket *vvs)
{
	mutex_lock(&sock_lock);

	TAILQ_REMOVE(&sockets_head, vvs, link);
	while (!TAILQ_EMPTY(&vvs->msgs)) {
		struct virtio_vsock_msg *m = NULL;
		struct virtq *vq = NULL;

		m = TAILQ_FIRST(&vvs->msgs);
		TAILQ_REMOVE(&vvs->msgs, m, link);
		switch (m->type) {
		case VIRTIO_VSOCKET_MSG_TYPE_DATA:
			free(m->data.buf);
			break;
		case VIRTIO_VSOCKET_MSG_TYPE_REQ:
			vq = m->vvs_req->dev->vdev.vqs + VIRTIO_VSOCK_VQ_IDX_RX;
			if (!reply_op(vq, m->vvs_req, VIRTIO_VSOCK_OP_RST)) {
				m->vvs_req->dead = true;
				m->vvs_req->pending_op = VIRTIO_VSOCK_OP_RST;
				TAILQ_INSERT_TAIL(&sockets_backlog_head,
						  m->vvs_req, link_backlog);
				break;
			}
			free(m->vvs_req);
			break;
		case VIRTIO_VSOCKET_MSG_TYPE_SHM:
		default:
			break;
		}
		free(m);
	}

	if (!vvs->listen &&
	    !reply_op(vvs->dev->vdev.vqs + VIRTIO_VSOCK_VQ_IDX_RX, vvs,
		      VIRTIO_VSOCK_OP_RST)) {
		vvs->owner = NULL;
		vvs->dead = true;
		vvs->pending_op = VIRTIO_VSOCK_OP_RST;
		TAILQ_INSERT_TAIL(&sockets_backlog_head, vvs, link_backlog);
	} else {
		free(vvs);
	}

	mutex_unlock(&sock_lock);
}

void virtio_vsock_msgq_lock(struct virtio_vsock_socket *vvs __unused)
{
	mutex_lock(&sock_lock);
}

void virtio_vsock_msgq_unlock(struct virtio_vsock_socket *vvs __unused)
{
	mutex_unlock(&sock_lock);
}

static struct virtio_vsock_msg *
find_msg_type(struct virtio_vsock_msg_head *msgs,
	      enum virtio_vsock_msg_type type)
{
	struct virtio_vsock_msg *m = NULL;

	TAILQ_FOREACH(m, msgs, link)
		if (m->type == type)
			return m;

	return NULL;
}

/* Returns pointer to head, doesn't remove, assumes lock held */
struct virtio_vsock_msg *
virtio_vsock_msgq_peek(struct virtio_vsock_socket *vvs,
		       enum virtio_vsock_msg_type type, uint32_t timeout)
{
	struct virtio_vsock_msg *m = find_msg_type(&vvs->msgs, type);
	uint64_t to = 0;
	int to_us = 0;

	if (timeout)
		to = timeout_init_us((uint64_t)timeout * 1000);

	while (!m && timeout) {
		to_us = timeout_elapsed_us(to);
		if (to_us >= 0)
			break;

		sock_vvs_msgs_wait_count++;
		condvar_wait_timeout(&sock_vvs_msgs_cv, &sock_lock,
				     -to_us / 1000);
		assert(sock_vvs_msgs_wait_count);
		sock_vvs_msgs_wait_count--;
		m = find_msg_type(&vvs->msgs, type);
	}

	return m;
}

/* Removes head, assumes lock held */
void virtio_vsock_msgq_dequeue(struct virtio_vsock_socket *vvs,
			       struct virtio_vsock_msg *m)
{
	assert(m);
	if (m->type == VIRTIO_VSOCKET_MSG_TYPE_DATA)
		vvs->rx_cnt += m->data.len;
	TAILQ_REMOVE(&vvs->msgs, m, link);
	free(m);
}

static TEE_Result vsock_check_send_buffer(struct virtio_vsock_socket *vvs,
					  size_t *sz, uint32_t timeout)
{
	uint32_t buf_cnt = 0;
	uint64_t to = 0;
	int to_us = 0;

	if (timeout)
		to = timeout_init_us((uint64_t)timeout * 1000);

	while (true) {
		if (process_backlog(vvs->dev->vdev.vqs +
				    VIRTIO_VSOCK_VQ_IDX_RX)) {
			if (vvs->local_tx_cnt > vvs->peer_fwd_cnt)
				buf_cnt = vvs->local_tx_cnt - vvs->peer_fwd_cnt;
			else
				buf_cnt = 0;

			if (vvs->peer_buf_alloc > buf_cnt) {
				*sz = MIN(*sz, vvs->peer_buf_alloc);
				return TEE_SUCCESS;
			}
		}

		if (timeout)
			to_us = timeout_elapsed_us(to);
		if (to_us >= 0)
			return TEE_ERROR_TIMEOUT;

		sock_vvs_send_wait_count++;
		condvar_wait_timeout(&sock_vvs_send_cv, &sock_lock,
				     -to_us / 1000);
		assert(sock_vvs_send_wait_count);
		sock_vvs_send_wait_count--;
	}
}

TEE_Result virtio_vsock_send(struct virtio_vsock_socket *vvs, const void *buf,
			     size_t *blen, uint32_t flags, uint32_t timeout)
{
	TEE_Result res = TEE_SUCCESS;
	struct virtq_write_ctx wbuf = { };
	struct virtio_vsock_hdr resp = {
		.src_cid = vvs->dst_cid,
		.dst_cid = vvs->src_cid,
		.src_port = vvs->dst_port,
		.dst_port = vvs->src_port,
		.type = vvs->type,
		.op = VIRTIO_VSOCK_OP_RW,
		.buf_alloc = vvs->buf_alloc,
		.fwd_cnt = vvs->fwd_cnt,
	};
	size_t sz = 0;

	if (vvs->type != VIRTIO_VSOCK_TYPE_SEQPACKET && flags)
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&sock_lock);
	sz = *blen;
	res = vsock_check_send_buffer(vvs, &sz, timeout);
	if (res) {
		DMSG("vsock_check_send_buffer: res %#"PRIx32, res);
		goto out;
	}

	resp.len = sz;
	/* Only set eventual flags if the entire buffer is transmitted */
	if (flags && sz == *blen)
		resp.flags = flags;

	res = virtq_write_start(vvs->dev->vdev.vqs + VIRTIO_VSOCK_VQ_IDX_RX,
				&wbuf, sizeof(resp) + sz);
	if (res) {
		DMSG("virtq_write_start: res %#"PRIx32, res);
		goto out;
	}

	res = virtq_write_copy(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		goto out;
	}

	res = virtq_write_copy(buf, &wbuf, sizeof(resp), sz);
	if (res) {
		DMSG("virtq_write_copy: res %#"PRIx32, res);
		goto out;
	}

	vvs->local_tx_cnt += sz;
	*blen = sz;
	virtq_write_finish(&wbuf, sizeof(resp) + sz);
out:
	mutex_unlock(&sock_lock);

	return res;
}

static const struct virtio_description vsock_desc = {
	.dev_id = VIRTIO_VSOCK_DEVICE_ID,
	.vendor_id = 0,
	.config_size = sizeof(((struct virtio_vsock_device *)0)->cid),
	.vq_count = VIRTIO_VSOCK_VQ_IDX_COUNT,
	.admin_vq_start_idx = 0,	/* VIRTIO_F_ADMIN_VQ not used */
	.admin_vq_count = 0,		/* VIRTIO_F_ADMIN_VQ not used */
	.feature_bit_count = VIRTIO_MAX_FEATURE_BIT_COUNT,
	.max_desc_count = 1024,
	.get_features = vsock_get_features,
	.set_features = vsock_set_features,
	.get_config = vsock_get_config,
	.set_virtque = vsock_set_virtque,
	.get_virtque = vsock_get_virtque,
};

static TEE_Result vsock_init(void)
{
	struct virtio_vsock_device *dev = calloc(1, sizeof(*dev));

	if (!dev)
		return TEE_ERROR_OUT_OF_MEMORY;

	dev->cid = 0; /* The non-secure physical end-point */
	dev->vdev.desc = &vsock_desc;

	return virtio_register_device(&dev->vdev);
}

nex_service_init(vsock_init);
