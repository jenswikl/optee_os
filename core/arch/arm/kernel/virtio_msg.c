// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <kernel/notif.h>
#include <kernel/thread_spmc.h>
#include <kernel/virtio.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <string.h>

#define VIRTIO_MSG_DEVICE_INFO		0x02
#define VIRTIO_MSG_GET_DEV_FEATURES	0x03
#define VIRTIO_MSG_SET_DRV_FEATURES	0x04
#define VIRTIO_MSG_GET_CONFIG		0x05
#define VIRTIO_MSG_SET_CONFIG		0x06
#define VIRTIO_MSG_GET_DEVICE_STATUS	0x07
#define VIRTIO_MSG_SET_DEVICE_STATUS	0x08
#define VIRTIO_MSG_GET_VQUEUE		0x09
#define VIRTIO_MSG_SET_VQUEUE		0x0a
#define VIRTIO_MSG_RESET_VQUEUE		0x0b
#define VIRTIO_MSG_GET_SHM		0x0c
#define VIRTIO_MSG_EVENT_CONFIG		0x40
#define VIRTIO_MSG_EVENT_AVAIL		0x41
#define VIRTIO_MSG_EVENT_USED		0x42

#define BUS_MSG_GET_DEVICES		0x02
#define BUS_MSG_PING			0x03
#define BUS_MSG_EVENT_DEVICE		0x40

#define MSG_TYPE_TRANSPORT_REQUEST	0x0
#define MSG_TYPE_TRANSPORT_RESPONSE	0x1
#define MSG_TYPE_BUS_REQUEST		0x2
#define MSG_TYPE_BUS_RESPONSE		0x3

#define FFA_BUS_MSG_VERSION		0x80
#define FFA_BUS_MSG_AREA_SHARE		0x81
#define FFA_BUS_MSG_AREA_UNSHARE	0x82
#define FFA_BUS_MSG_RESET		0x83
#define FFA_BUS_MSG_EVENT_POLL		0x84
#define FFA_BUS_MSG_EVENT_CONFIGURE	0x85
#define FFA_BUS_EVENT_AREA_RELEASE	0xC0

#define FFA_PROTO_VERSION		1
#define VMSG_REVISION			1
#define VMSG_FEATURES			0

#define FFA_BUS_FEATURE_DIRECT_MSG_RX	BIT(0)
#define FFA_BUS_FEATURE_DIRECT_MSG_TX	BIT(1)
#define FFA_BUS_FEATURE_INDIRECT_MSG_RX	BIT(2)
#define FFA_BUS_FEATURE_INDIRECT_MSG_TX	BIT(3)

#define FFA_MSG_SIZE			96

struct virtio_msg_hdr {
	uint8_t type;
	uint8_t msg_id;
	uint16_t dev_num;
	uint16_t msg_size;
};

struct virtio_msg_version {
	uint32_t proto_vers;
	uint32_t vmsg_rev;
	uint32_t vmsg_features;
	uint32_t features;
	uint16_t area_max_count;
} __packed __aligned(2);

struct virtio_msg_get_devices_req {
	uint16_t offset;
	uint16_t count;
};

struct virtio_msg_get_devices_resp {
	uint16_t offset;
	uint16_t count;
	uint16_t next_offset;
};

struct virtio_msg_ffa_bus_msg_area_share_req {
	uint16_t area_id;
	uint64_t handle;
	uint64_t tag;
	uint32_t page_count;
	uint32_t attrs;
} __packed __aligned(2);

struct virtio_msg_ffa_bus_msg_area_share_resp {
	uint16_t area_id;
	uint16_t result;
};

struct virtio_msg_ffa_bus_msg_area_unshare_req {
	uint16_t area_id;
};

struct virtio_msg_ffa_bus_msg_area_unshare_resp {
	uint16_t area_id;
	uint16_t result;
};

struct virtio_msg_get_device_info_resp {
	uint32_t device_id;
	uint32_t vendor_id;
	uint32_t feature_bit_count;
	uint32_t config_size;
	uint32_t max_vq_count;
	uint16_t admin_vq_start_idx;
	uint16_t admin_vq_count;
} __packed __aligned(2);

struct virtio_msg_get_dev_features {
	uint32_t block_idx;
	uint32_t block_count;;
} __packed __aligned(2);

struct virtio_msg_set_drv_features {
	uint32_t block_idx;
	uint32_t block_count;;
} __packed __aligned(2);

struct virtio_msg_get_config_req {
	uint32_t byte_offset;
	uint32_t byte_count;
} __packed __aligned(2);

struct virtio_msg_get_config_resp {
	uint32_t gen_count;;
	uint32_t byte_offset;
	uint32_t byte_count;
} __packed __aligned(2);

struct virtio_msg_device_status {
	uint32_t status;
} __packed __aligned(2);

struct virtio_msg_get_vqueue_req {
	uint32_t vq_idx;
} __packed __aligned(2);

struct virtio_msg_get_vqueue_resp {
	uint32_t vq_idx;
	uint32_t max_size;
	uint32_t vq_size;
	uint64_t desc_addr;
	uint64_t driver_addr;
	uint64_t device_addr;
} __packed __aligned(2);

struct virtio_msg_set_vqueue_req {
	uint32_t vq_idx;
	uint32_t reserved;
	uint32_t vq_size;
	uint64_t desc_addr;
	uint64_t driver_addr;
	uint64_t device_addr;
} __packed __aligned(2);

struct virtio_msg_event_avail {
	uint32_t vq_idx;
	uint32_t next_wrap;
} __packed __aligned(2);

struct virtio_vsock_config {
	uint64_t guest_cid;
};

static void set_null_msg(struct virtio_msg_hdr *hdr)
{
	*hdr = (struct virtio_msg_hdr){
		.type = MSG_TYPE_BUS_RESPONSE,
		.msg_size = 6,
	};
}

static void handle_bus_msg_version(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_version *v = (void *)(hdr + 1);
	size_t msg_size = sizeof(*hdr) + sizeof(*v);

	if (hdr->dev_num || hdr->msg_size != msg_size) {
		hdr->msg_size = msg_size;
		hdr->dev_num = 0;
		memset(v, 0, sizeof(*v));
		return;
	}

	if (v->proto_vers != FFA_PROTO_VERSION)
		v->proto_vers = 0;
	if (v->vmsg_rev != VMSG_REVISION)
		v->vmsg_rev = 0;
	if (v->vmsg_features != VMSG_FEATURES)
		v->vmsg_features = 0;
	v->features = FFA_BUS_FEATURE_DIRECT_MSG_RX;
	v->area_max_count = 0xff;
}

static void handle_bus_msg_get_devices(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_get_devices_req *req = (void *)(hdr + 1);
	struct virtio_msg_get_devices_resp *resp = (void *)(hdr + 1);
	bitstr_t *bs = (void *)(resp + 1);
	size_t req_count = 0;
	size_t pop_count = 0;

	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req))
		goto err;

	/* We only support offset 0 */
	if (req->offset)
		goto err;

#if 0 //Kernel uses 0xff
	if (req->count % 8)
		goto err;
#endif

	if (sizeof(*hdr) + sizeof(*resp) + req->count / 8 > FFA_MSG_SIZE)
		goto err;

	req_count = req->count;
	if (virtio_get_devices_bitstring(bs, req_count, &pop_count))
		goto err;
	resp->offset = 0;
	resp->count = pop_count;
	resp->next_offset = 0;
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp) + req_count / 8;

	return;
err:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
	hdr->dev_num = 0;
	memset(resp, 0, sizeof(*resp));
}

static void handle_ffa_bus_msg_area_share(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_ffa_bus_msg_area_share_req *req = (void *)(hdr + 1);
	struct virtio_msg_ffa_bus_msg_area_share_resp *resp = (void *)(hdr + 1);
	uint64_t handle = req->handle;
	uint16_t area_id = req->area_id;

	memset(resp, 0, sizeof(*resp));
	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req)) {
		hdr->dev_num = 0;
		goto out;
	}
	resp->area_id = area_id;

	if (virtio_area_share_ffa(area_id, handle))
		resp->result = 1;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_ffa_bus_msg_area_unshare(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_ffa_bus_msg_area_unshare_req *req = (void *)(hdr + 1);
	struct virtio_msg_ffa_bus_msg_area_unshare_resp *resp = (void *)req;
	uint16_t area_id = req->area_id;

	memset(resp, 0, sizeof(*resp));
	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req)) {
		hdr->dev_num = 0;
		goto out;
	}
	resp->area_id = area_id;

	if (virtio_area_unshare(area_id))
		resp->result = 1;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void recv_bus_req(struct virtio_msg_hdr *hdr)
{
	switch (hdr->msg_id) {
	case FFA_BUS_MSG_VERSION:
		handle_bus_msg_version(hdr);
		break;
	case BUS_MSG_GET_DEVICES:
		handle_bus_msg_get_devices(hdr);
		break;
	case FFA_BUS_MSG_AREA_SHARE:
		handle_ffa_bus_msg_area_share(hdr);
		break;
	case FFA_BUS_MSG_AREA_UNSHARE:
		handle_ffa_bus_msg_area_unshare(hdr);
		break;
	default:
		assert(0);
		set_null_msg(hdr);
	}
}

static void handle_msg_get_device_info(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_get_device_info_resp *resp = (void *)(hdr + 1);

	memset(resp, 0, sizeof(*resp));
	if (hdr->msg_size != sizeof(*hdr))
		goto out;

	resp->device_id = 19; /* socket device */
	resp->feature_bit_count = 64; /* The linux driver expects this */
	resp->config_size = sizeof(struct virtio_vsock_config);
	resp->max_vq_count = 2;
	resp->admin_vq_start_idx = 1;
	resp->admin_vq_count = 1;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_msg_get_dev_features(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_get_dev_features *v = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;
	size_t block_size = 32;
	size_t count = 0;
	size_t offs = 0;

	if (hdr->msg_size != sizeof(*hdr) + sizeof(*v))
		goto err;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (!vdev)
		goto err;

	count = v->block_count * block_size;
	offs = v->block_idx * block_size;
	if (count > VIRTIO_MAX_FEATURE_BIT_COUNT ||
	    offs > VIRTIO_MAX_FEATURE_BIT_COUNT ||
	    count + offs > VIRTIO_MAX_FEATURE_BIT_COUNT)
		goto err;

	memset(v + 1, 0, v->block_count * sizeof(uint32_t));
	vdev->desc->get_features(vdev, (void *)(v + 1), count, offs);
	hdr->msg_size = sizeof(*hdr) + sizeof(*v) +
			v->block_count * sizeof(uint32_t);
	return;
err:
	memset(v, 0, sizeof(*v));
	hdr->msg_size = sizeof(*hdr) + sizeof(*v);
}

static void handle_msg_set_drv_features(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_set_drv_features *v = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;
	size_t block_size = 32;
	size_t count = 0;
	size_t offs = 0;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (!vdev)
		goto out;
	if (hdr->msg_size != sizeof(*hdr) + sizeof(*v) +
			     v->block_count * sizeof(uint32_t)) {
		vdev->features_ok = false;
		goto out;
	}

	count = v->block_count * block_size;
	offs = v->block_idx * block_size;
	if (count > VIRTIO_MAX_FEATURE_BIT_COUNT ||
	    offs > VIRTIO_MAX_FEATURE_BIT_COUNT ||
	    count + offs > VIRTIO_MAX_FEATURE_BIT_COUNT) {
		vdev->features_ok = false;
		goto out;
	}

	vdev->desc->set_features(vdev, (void *)(v + 1), count, offs);
out:
	hdr->msg_size = sizeof(*hdr);
}

static void handle_msg_get_config(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_get_config_req *req = (void *)(hdr + 1);
	struct virtio_msg_get_config_resp *resp = (void *)(hdr + 1);
	uint32_t byte_offset = req->byte_offset;
	uint32_t byte_count = req->byte_count;
	struct virtio_device *vdev = NULL;

	memset(resp, 0, sizeof(*resp));
	if (hdr->msg_size != sizeof(*hdr) + sizeof(*req))
		goto out;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (!vdev)
		goto out;

	if (byte_offset > vdev->desc->config_size ||
	    byte_count > vdev->desc->config_size ||
	    byte_offset + byte_count > vdev->desc->config_size)
		goto out;

	resp->gen_count = vdev->conf_gen_count;
	resp->byte_offset = byte_offset;
	resp->byte_count = byte_count;
	vdev->desc->get_config(vdev, resp + 1, byte_count, byte_offset);
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp) + resp->byte_count;
}

static void handle_msg_set_device_status(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_device_status *v = (void *)(hdr + 1);
	size_t msg_size = sizeof(*hdr) + sizeof(*v);
	struct virtio_device *vdev = NULL;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (hdr->msg_size != msg_size || !vdev) {
		DMSG("hdr->msg_size %#"PRIx16" vs msg_size %#zx vdev %p",
		     hdr->msg_size, msg_size, (void *)vdev);
		hdr->msg_size = msg_size;
		memset(v, 0, sizeof(*v));
		return;
	}

	DMSG("v->status %#"PRIx32, v->status);
	if (!v->status && vdev->status)
		panic("TODO reset device");
	if (v->status & VIRTIO_DEV_STATUS_FAILED)
		goto out;
	if (vdev->status & VIRTIO_DEV_STATUS_DEVICE_NEEDS_RESET)
		goto out;

	if (v->status & VIRTIO_DEV_STATUS_ACKNOWLEDGE) {
		vdev->status |= VIRTIO_DEV_STATUS_ACKNOWLEDGE;
		DMSG("VIRTIO_DEV_STATUS_ACKNOWLEDGE");
	}

	if (v->status & VIRTIO_DEV_STATUS_DRIVER) {
		if (!(vdev->status & VIRTIO_DEV_STATUS_ACKNOWLEDGE))
			goto out;
		vdev->status |= VIRTIO_DEV_STATUS_DRIVER;
		DMSG("VIRTIO_DEV_STATUS_DRIVER");
	}

	if (v->status & VIRTIO_DEV_STATUS_FEATURES_OK) {
		if (!(vdev->status & VIRTIO_DEV_STATUS_DRIVER) ||
		    !vdev->features_ok)
			goto out;
		vdev->status |= VIRTIO_DEV_STATUS_FEATURES_OK;
		DMSG("VIRTIO_DEV_STATUS_FEATURES_OK");
	}

	if (v->status & VIRTIO_DEV_STATUS_DRIVER_OK) {
		if (!(vdev->status & VIRTIO_DEV_STATUS_FEATURES_OK))
			goto out;
		vdev->status |= VIRTIO_DEV_STATUS_DRIVER_OK;
		DMSG("VIRTIO_DEV_STATUS_DRIVER_OK");
	}

out:
	v->status = vdev->status;
}

static void handle_msg_get_device_status(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_device_status *resp = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (hdr->msg_size != sizeof(*hdr) || !vdev) {
		DMSG("hdr->msg_size %#"PRIx16" vs msg_size %#zx vdev %p",
		     hdr->msg_size, sizeof(*hdr), (void *)vdev);
		memset(resp, 0, sizeof(*resp));
		hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
		hdr->dev_num = 0;
		return;
	}

	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
	resp->status = vdev->status;
}

static void handle_msg_get_vqueue(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_get_vqueue_req *req = (void *)(hdr + 1);
	struct virtio_msg_get_vqueue_resp *resp = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;
	uint32_t vq_idx = req->vq_idx;
	struct virtq *vq = NULL;

	vdev = virtio_lookup_device(hdr->dev_num);
	memset(resp, 0, sizeof(*resp));
	if (hdr->msg_size != sizeof(*hdr) + sizeof(*req) || !vdev) {
		hdr->dev_num = 0;
		goto out;
	}

	resp->vq_idx = vq_idx;
	vq = vdev->desc->get_virtque(vdev, vq_idx);
	if (vq) {
		resp->max_size = vdev->desc->max_desc_count /
				 sizeof(struct virtq_desc);
		resp->vq_size = vq->desc_count;
		resp->desc_addr = virt_to_phys(vq->desc);
		resp->driver_addr = virt_to_phys(vq->driver);
		resp->device_addr = virt_to_phys(vq->device);
	}

out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_msg_set_vqueue(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_set_vqueue_req *req = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;
	TEE_Result res = TEE_SUCCESS;
	struct virtq *vq = NULL;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (hdr->msg_size != sizeof(*hdr) + sizeof(*req) || !vdev) {
		hdr->dev_num = 0;
		goto out;
	}
	vq = vdev->desc->get_virtque(vdev, req->vq_idx);
	if (!vq)
		goto out;
	res = virtq_init(vq, req->vq_size, req->desc_addr, req->driver_addr,
			 req->device_addr);
	if (res)
		goto out;
	vdev->desc->set_virtque(vdev, vq);
out:
	hdr->msg_size = sizeof(*hdr);
}

static void handle_msg_event_avail(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_event_avail *event = (void *)(hdr + 1);
	struct virtio_device *vdev = NULL;
	struct virtq *vq = NULL;

	vdev = virtio_lookup_device(hdr->dev_num);
	if (hdr->msg_size != sizeof(*hdr) + sizeof(*event) || !vdev) {
		hdr->dev_num = 0;
		goto out;
	}
	vq = vdev->desc->get_virtque(vdev, event->vq_idx);
	if (vq && vq->enabled)
		notif_send_async(NOTIF_VALUE_DO_BOTTOM_HALF, 0);

out:
	hdr->msg_size = sizeof(*hdr);
}

static void recv_transport_req(struct virtio_msg_hdr *hdr)
{
	switch (hdr->msg_id) {
	case VIRTIO_MSG_DEVICE_INFO:
		handle_msg_get_device_info(hdr);
		break;
	case VIRTIO_MSG_GET_DEV_FEATURES:
		handle_msg_get_dev_features(hdr);
		break;
	case VIRTIO_MSG_SET_DRV_FEATURES:
		handle_msg_set_drv_features(hdr);
		break;
	case VIRTIO_MSG_GET_CONFIG:
		handle_msg_get_config(hdr);
		break;
	case VIRTIO_MSG_GET_DEVICE_STATUS:
		handle_msg_get_device_status(hdr);
		break;
	case VIRTIO_MSG_SET_DEVICE_STATUS:
		handle_msg_set_device_status(hdr);
		break;
	case VIRTIO_MSG_GET_VQUEUE:
		handle_msg_get_vqueue(hdr);
		break;
	case VIRTIO_MSG_SET_VQUEUE:
		handle_msg_set_vqueue(hdr);
		break;
	case VIRTIO_MSG_EVENT_AVAIL:
		handle_msg_event_avail(hdr);
		break;
	default:
		assert(0);
		set_null_msg(hdr);
	}
}

void virtio_msg_recv(struct thread_smc_1_2_regs *args)
{
	struct virtio_msg_hdr *hdr = (struct virtio_msg_hdr *)(args->a + 4);

	DMSG("hdr type %#x msg_id %#x dev_num %#x msg_size %#x",
		hdr->type, hdr->msg_id, hdr->dev_num, hdr->msg_size);
	switch (hdr->type) {
	case MSG_TYPE_BUS_REQUEST:
		recv_bus_req(hdr);
		hdr->type = MSG_TYPE_BUS_RESPONSE;
		break;
	case MSG_TYPE_TRANSPORT_REQUEST:
		recv_transport_req(hdr);
		hdr->type = MSG_TYPE_TRANSPORT_RESPONSE;
		break;
	default:
		assert(0);
		set_null_msg(hdr);
	}
	assert(hdr->msg_size <= FFA_MSG_SIZE);
	memset((uint8_t *)hdr + hdr->msg_size, 0, FFA_MSG_SIZE - hdr->msg_size);
}
