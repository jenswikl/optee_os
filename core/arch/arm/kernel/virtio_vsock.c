// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <bitstring.h>
#include <kernel/virtio.h>
#include <string.h>
#include <mm/core_mmu.h>

#include <confine_array_index.h> // Nedded?

struct vsock_socket {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint16_t type;
	TAILQ_ENTRY(vsock_socket) link;
};
TAILQ_HEAD(vsock_socket_head, vsock_socket);

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

enum vsock_idx {
	VSOCK_VQ_IDX_RX = 0,
	VSOCK_VQ_IDX_TX,
	VSOCK_VQ_IDX_EVENT,
};

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

#define VIRTIO_VSOCK_TYPE_STREAM	1
#define VIRTIO_VSOCK_TYPE_SEQPACKET	2

#define VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE	0
#define VIRTIO_VSOCK_SHUTDOWN_F_SEND	1

static const struct virtio_description vsock_desc;
static const uint8_t vsock_features[] = {
	VIRTIO_F_VERSION_1,
	VIRTIO_VSOCK_F_STREAM,
	VIRTIO_VSOCK_F_SEQPACKET,
	VIRTIO_VSOCK_F_NO_IMPLIED_STREAM,
	VIRTIO_F_ACCESS_PLATFORM,
};

static struct vsock_socket_head sockets_head =
	TAILQ_HEAD_INITIALIZER(sockets_head);

static bool cmp_socket(const struct vsock_socket *vss1,
		       const struct vsock_socket *vss2)
{
	return vss1->src_cid == vss2->src_cid &&
	       vss1->dst_cid == vss2->dst_cid &&
	       vss1->src_port == vss2->src_port &&
	       vss1->dst_port == vss2->dst_port;
}

static struct vsock_socket *find_socket(const struct vsock_socket *vss)
{
	struct vsock_socket *v = NULL;

	TAILQ_FOREACH(v, &sockets_head, link)
		if (cmp_socket(v, vss))
			return v;

	return NULL;
}

static struct vsock_socket *find_socket2(uint64_t src_cid, uint64_t dst_cid,
					 uint32_t src_port, uint32_t dst_port)
{
	struct vsock_socket vss = {
		.src_cid = src_cid,
		.dst_cid = dst_cid,
		.src_port = src_port,
		.dst_port = dst_port,
	};

	return find_socket(&vss);
}

static TEE_Result add_socket(struct vsock_socket *vss)
{
	if (find_socket(vss))
		return TEE_ERROR_ACCESS_CONFLICT;

	TAILQ_INSERT_TAIL(&sockets_head, vss, link);

	return TEE_SUCCESS;
}

static void rem_socket(struct vsock_socket *vss)
{
	TAILQ_REMOVE(&sockets_head, vss, link);
}

static struct virtio_vsock *vdev_to_vsock(struct virtio_device *vdev)
{
	assert(vdev->desc == &vsock_desc);

	return container_of(vdev, struct virtio_vsock, vdev);
}

static void handle_op_request(struct virtq_rbuf *vqr,
			      struct virtio_vsock_hdr *req)
{
	TEE_Result res = TEE_SUCCESS;
	struct virtq_wbuf wbuf = { };
	struct vsock_socket *vss = NULL;
	struct virtio_vsock_hdr resp = {
		.src_cid = req->dst_cid,
		.dst_cid = req->src_cid,
		.src_port = req->dst_port,
		.dst_port = req->src_port,
		.type = req->type,
		.op = VIRTIO_VSOCK_OP_RESPONSE,
		.buf_alloc = 1024,
	};

	if (req->type != VIRTIO_VSOCK_TYPE_STREAM &&
	    req->type != VIRTIO_VSOCK_TYPE_SEQPACKET) {
		/* Unknown type */
		DMSG("Unknown type");
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}

	vss = calloc(1, sizeof(*vss));
	if (!vss) {
		/* Can't establish a new connection */
		DMSG("Calloc failed");
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}
	vss->src_cid = req->src_cid;
	vss->dst_cid = req->dst_cid,
	vss->src_port = req->src_port;
	vss->dst_port = req->dst_port;
	if (add_socket(vss)) {
		/* Connection already exists */
		DMSG("Connection already exists");
		free(vss);
		resp.op = VIRTIO_VSOCK_OP_RST;
		goto out;
	}

out:
	res = virtq_payload_write_init(vqr->vq->vdev->vqs + VSOCK_VQ_IDX_RX,
				       &wbuf, sizeof(resp));
	if (res) {
		DMSG("virtq_payload_write_init: res %#"PRIx32, res);
		return;
	}

	res = virtq_copy_to_wbuf(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_copy_to_wbuf: res %#"PRIx32, res);
		return;
	}

	virtq_wbuf_deliver(&wbuf, sizeof(resp));
}

static void handle_op_rw(struct virtq_rbuf *vqr, struct virtio_vsock_hdr *req)
{
	struct vsock_socket *vss = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_wbuf wbuf = { };
	struct virtio_vsock_hdr resp = {
		.src_cid = req->dst_cid,
		.dst_cid = req->src_cid,
		.src_port = req->dst_port,
		.dst_port = req->src_port,
		.len = req->len,
		.type = req->type,
		.op = VIRTIO_VSOCK_OP_RW,
		.buf_alloc = 1024,
	};
	uint8_t buf[32] = { };
	size_t n = 0;
	size_t l = 0;

	res = virtq_payload_write_init(vqr->vq->vdev->vqs + VSOCK_VQ_IDX_RX,
				       &wbuf, sizeof(resp) + req->len);
	if (res) {
		DMSG("virtq_payload_write_init: res %#"PRIx32, res);
		return;
	}

	vss = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port);
	if (!vss) {
		/* Connection not found */
		DMSG("Connection not found");
		resp.len = 0;
		resp.op = VIRTIO_VSOCK_OP_RST;
		resp.buf_alloc = 0;
		goto out;
	}

	res = virtq_copy_to_wbuf(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_copy_to_wbuf: res %#"PRIx32, res);
		return;
	}

	while (n < req->len) {
		l = MIN(sizeof(buf), req->len - n);
		res = virtq_copy_from_rbuf(buf, vqr, sizeof(*req) + n, l);
		if (res) {
			DMSG("virtq_copy_from_rbuf: res %#"PRIx32, res);
			return;
		}
		res = virtq_copy_to_wbuf(buf, &wbuf, sizeof(resp) + n, l);
		if (res) {
			DMSG("virtq_copy_to_wbuf: res %#"PRIx32, res);
			return;
		}
		n += l;
	}

out:
	virtq_wbuf_deliver(&wbuf, sizeof(resp) + req->len);
}

static void handle_op_shutdown(struct virtq_rbuf *vqr,
			       struct virtio_vsock_hdr *req)
{
	struct vsock_socket *vss = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_wbuf wbuf = { };
	struct virtio_vsock_hdr resp = {
		.src_cid = req->dst_cid,
		.dst_cid = req->src_cid,
		.src_port = req->dst_port,
		.dst_port = req->src_port,
		.type = req->type,
		.op = VIRTIO_VSOCK_OP_SHUTDOWN,
		.flags = BIT(VIRTIO_VSOCK_SHUTDOWN_F_RECEIVE) |
			 BIT(VIRTIO_VSOCK_SHUTDOWN_F_SEND),
	};

	res = virtq_payload_write_init(vqr->vq->vdev->vqs + VSOCK_VQ_IDX_RX,
				       &wbuf, sizeof(resp) + req->len);
	if (res) {
		DMSG("virtq_payload_write_init: res %#"PRIx32, res);
		return;
	}

	vss = find_socket2(req->src_cid, req->dst_cid, req->src_port,
			   req->dst_port);
	if (!vss) {
		/* Connection not found */
		DMSG("Connection not found");
		resp.op = VIRTIO_VSOCK_OP_RST;
		resp.buf_alloc = 0;
		resp.flags = 0;
		goto out;
	}
	rem_socket(vss);
	free(vss);

out:
	res = virtq_copy_to_wbuf(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_copy_to_wbuf: res %#"PRIx32, res);
		return;
	}

	virtq_wbuf_deliver(&wbuf, sizeof(resp) + req->len);
}

static void handle_op_rst(struct virtq_rbuf *vqr,
			       struct virtio_vsock_hdr *req)
{
	struct vsock_socket *vss = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq_wbuf wbuf = { };
	struct virtio_vsock_hdr resp = {
		.src_cid = req->dst_cid,
		.dst_cid = req->src_cid,
		.src_port = req->dst_port,
		.dst_port = req->src_port,
		.type = req->type,
		.op = VIRTIO_VSOCK_OP_RST,
	};

	res = virtq_payload_write_init(vqr->vq->vdev->vqs + VSOCK_VQ_IDX_RX,
				       &wbuf, sizeof(resp) + req->len);
	if (res) {
		DMSG("virtq_payload_write_init: res %#"PRIx32, res);
		return;
	}

	vss = find_socket2(req->dst_cid, req->src_cid, req->dst_port,
			   req->src_port);
	if (vss) {
		DMSG("Removing socket");
		rem_socket(vss);
		free(vss);
	}

	res = virtq_copy_to_wbuf(&resp, &wbuf, 0, sizeof(resp));
	if (res) {
		DMSG("virtq_copy_to_wbuf: res %#"PRIx32, res);
		return;
	}

	virtq_wbuf_deliver(&wbuf, sizeof(resp) + req->len);
}

static void handle_rx_payload(struct virtq_rbuf *vqr)
{
	struct virtio_vsock_hdr hdr = { };
	TEE_Result res = TEE_SUCCESS;

	res = virtq_copy_from_rbuf(&hdr, vqr, 0, sizeof(hdr));
	if (res) {
		DMSG("virtq_copy_from_rbuf: res %#"PRIx32, res);
		return;
	}
	DMSG("src_cid %#"PRIx64" dst_cid %#"PRIx64,
	     hdr.src_cid, hdr.dst_cid);
	DMSG("src_port %#"PRIx32" dst_port %#"PRIx32,
	     hdr.src_port, hdr.dst_port);
	DMSG("len %"PRIu32" type %"PRIu16" op %"PRIu16,
	     hdr.len, hdr.type, hdr.op);
	DMSG("flags %#"PRIx32" buf_alloc %"PRIu32" fwd_cnt %"PRIu32,
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
		DMSG("Ignoring VIRTIO_VSOCK_OP_CREDIT_UPDATE");
		break;
	case VIRTIO_VSOCK_OP_CREDIT_REQUEST:
		DMSG("Ignoring VIRTIO_VSOCK_OP_CREDIT_REQUEST");
		break;
	default:
		DMSG("Unknown op %"PRIu16, hdr.op);
	}
}

static void vsock_vq_host_rx_callback(struct virtq *vq)
{
	struct virtq_rbuf vqr = { };

	DMSG("idx %u", vq->idx);

	while (!virtq_payload_read_init(vq, &vqr)) {
		handle_rx_payload(&vqr);
		virtq_rbuf_consume(&vqr);
	}
}

static void vsock_vq_host_tx_callback(struct virtq *vq)
{
	DMSG("idx %u", vq->idx);
}

static void vsock_vq_event_callback(struct virtq *vq)
{
	DMSG("idx %u", vq->idx);
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
	struct virtio_vsock *vsock = vdev_to_vsock(vdev);
	uint8_t *vsock_config = (uint8_t *)&vsock->cid;

	assert(sizeof(vsock->cid) == vsock_desc.config_size);
	assert(count + offset <= sizeof(vsock->cid));

	memcpy(data, vsock_config + offset, count);
}

static void vsock_set_virtque(struct virtio_device *vdev, struct virtq *vq)
{
	if (!vq->vdev) {
		vq->vdev = vdev;
		switch (vq->idx) {
		case VSOCK_VQ_IDX_RX:
			vq->callback = vsock_vq_host_tx_callback;
			break;
		case VSOCK_VQ_IDX_TX:
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


static const struct virtio_description vsock_desc = {
	.dev_id = 19,
	.vendor_id = 0,
	.config_size = sizeof(uint64_t), /* struct virtio_vsock::cid */
	.vq_count = 3,
	.admin_vq_start_idx = 2,
	.admin_vq_count = 1,
	.shm_seg_count = 10, /* XXX */
	.feature_bit_count = VIRTIO_MAX_FEATURE_BIT_COUNT,
	.max_desc_count = SMALL_PAGE_SIZE / 4, // TODO 256
	.get_features = vsock_get_features,
	.set_features = vsock_set_features,
	.get_config = vsock_get_config,
	.set_virtque = vsock_set_virtque,
	.get_virtque = vsock_get_virtque,
};

TEE_Result virtio_register_vsock(struct virtio_vsock *vsock)
{
	/* TODO check against vsock->cid conflict */

	vsock->vdev.desc = &vsock_desc;

	return virtio_register_device(&vsock->vdev);
}
