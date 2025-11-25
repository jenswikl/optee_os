// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <kernel/spinlock.h>
#include <kernel/thread_spmc.h>
#include <kernel/virtio.h>
#include <kernel/virtio_msg.h>
#include <kernel/virtio_msg_ffa.h>

#define VIRTIO_MSG_FFA_AREA_ID_MASK	0xFFFF
#define VIRTIO_MSG_FFA_AREA_ID_SHIFT	48

#define FFA_BUS_MSG_VERSION		0x80
#define FFA_BUS_MSG_AREA_SHARE		0x81
#define FFA_BUS_MSG_AREA_UNSHARE	0x82
#define FFA_BUS_MSG_RESET		0x83
#define FFA_BUS_MSG_EVENT_POLL		0x84
#define FFA_BUS_MSG_EVENT_CONFIGURE	0x85
#define FFA_BUS_EVENT_AREA_RELEASE	0xC0

#define FFA_BUS_FEATURE_DIRECT_MSG_RX	BIT(0)
#define FFA_BUS_FEATURE_DIRECT_MSG_TX	BIT(1)
#define FFA_BUS_FEATURE_INDIRECT_MSG_RX	BIT(2)
#define FFA_BUS_FEATURE_INDIRECT_MSG_TX	BIT(3)
#define FFA_BUS_FEATURE_FFA_NOTIF_RX	BIT(4)
#define FFA_BUS_FEATURE_FFA_NOTIF_TX	BIT(5)
#define FFA_BUS_FEATURE_FIFO		BIT(6)

#define FFA_MSG_SIZE			96

#define FFA_PROTO_VERSION		0x00010000
#define VMSG_REVISION			1
#define VMSG_FEATURES			0

struct virtio_msg_version_req {
	uint32_t proto_vers;
	uint32_t vmsg_rev;
};

struct virtio_msg_version_resp {
	uint32_t proto_vers;
	uint32_t vmsg_rev;
	uint32_t vmsg_features;
	uint32_t features;
	uint16_t area_max_count;
} __packed __aligned(2);

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

struct virtio_msg_ffa_bus_msg_reset_resp {
	uint16_t result;
};

struct virtio_msg_ffa_bus_msg_event_config_req {
	uint8_t type;
	uint8_t reserved;
	uint16_t notify_id;
};

struct virtio_msg_ffa_bus_msg_event_config_resp {
	uint16_t result;
};

struct virtio_area {
	struct mobj *mobj;
	uint64_t handle;
	uint16_t id;
	TAILQ_ENTRY(virtio_area) link;
};

TAILQ_HEAD(virtio_area_head, virtio_area);
static struct virtio_area_head areas_head = TAILQ_HEAD_INITIALIZER(areas_head);
unsigned int areas_lock = SPINLOCK_UNLOCK;

static TEE_Result virtio_area_share(uint16_t area_id, uint64_t handle)
{
	struct virtio_area *a = NULL;
	uint32_t state = 0;

	a = calloc(1, sizeof(*a));
	if (!a)
		return TEE_ERROR_OUT_OF_MEMORY;

	a->mobj = mobj_ffa_get_by_cookie(handle, 0);
	if (!a->mobj)
		goto err_free_a;

	a->handle = handle;
	a->id = area_id;

	state = cpu_spin_lock_xsave(&areas_lock);
	TAILQ_INSERT_TAIL(&areas_head, a, link);
	cpu_spin_unlock_xrestore(&areas_lock, state);

	return TEE_SUCCESS;

err_free_a:
	free(a);

	return TEE_ERROR_GENERIC;
}

static struct virtio_area *find_virtio_area(uint16_t area_id)
{
	struct virtio_area *a = NULL;

	TAILQ_FOREACH(a, &areas_head, link)
		if (a->id == area_id)
			return a;

	return NULL;
}

static TEE_Result virtio_area_unshare(uint16_t area_id)
{
	TEE_Result res = TEE_ERROR_ITEM_NOT_FOUND;
	struct virtio_area *a = NULL;
	uint32_t state = 0;

	state = cpu_spin_lock_xsave(&areas_lock);
	a = find_virtio_area(area_id);
	if (a) {
		res = mobj_ffa_unregister_by_cookie(a->handle);
		if (!res || res == TEE_ERROR_ITEM_NOT_FOUND)
			TAILQ_REMOVE(&areas_head, a, link);
	}
	cpu_spin_unlock_xrestore(&areas_lock, state);

	if (!res)
		free(a);

	return res;
}

static uint16_t area_id_from_bus_addr(uint64_t bus_addr)
{
	return (bus_addr >> VIRTIO_MSG_FFA_AREA_ID_SHIFT) &
	       VIRTIO_MSG_FFA_AREA_ID_MASK;
}

TEE_Result virtio_bus_addr_inc_map(uint64_t bus_addr)
{
	TEE_Result res = TEE_SUCCESS;
	struct virtio_area *a = NULL;
	uint32_t state = 0;

	state = cpu_spin_lock_xsave(&areas_lock);
	a = find_virtio_area(area_id_from_bus_addr(bus_addr));
	if (a)
		mobj_get(a->mobj);
	cpu_spin_unlock_xrestore(&areas_lock, state);

	if (!a)
		return TEE_ERROR_BAD_PARAMETERS;

	res = mobj_inc_map(a->mobj);
	if (res)
		mobj_put(a->mobj);

	return res;
}

void virtio_bus_addr_dec_map(uint64_t bus_addr)
{
	struct virtio_area *a = NULL;
	uint32_t state = 0;

	state = cpu_spin_lock_xsave(&areas_lock);
	a = find_virtio_area(area_id_from_bus_addr(bus_addr));
	cpu_spin_unlock_xrestore(&areas_lock, state);

	if (a && !mobj_dec_map(a->mobj))
		mobj_put(a->mobj);
}

void *virtio_bus_addr_to_virt(uint64_t bus_addr, size_t len)
{
	struct virtio_area *a = NULL;
	uint16_t area_id = 0;
	uint32_t state = 0;
	size_t offs = 0;

	area_id = area_id_from_bus_addr(bus_addr);
	offs = bus_addr & ~SHIFT_U64(VIRTIO_MSG_FFA_AREA_ID_MASK,
				     VIRTIO_MSG_FFA_AREA_ID_SHIFT);

	state = cpu_spin_lock_xsave(&areas_lock);
	a = find_virtio_area(area_id);
	cpu_spin_unlock_xrestore(&areas_lock, state);
	if (!a)
		return NULL;

	return mobj_get_va(a->mobj, offs, len);
}

static void handle_ffa_bus_msg_version(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_version_req *req = (void *)(hdr + 1);
	struct virtio_msg_version_resp *resp = (void *)(hdr + 1);

	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req) ||
	    req->proto_vers != FFA_PROTO_VERSION ||
	    req->vmsg_rev != VMSG_REVISION) {
		hdr->dev_num = 0;
		memset(resp, 0, sizeof(*resp));
		goto out;
	}

	memset(resp, 0, sizeof(*resp));
	resp->proto_vers = FFA_PROTO_VERSION;
	resp->vmsg_rev = VMSG_REVISION;
	resp->vmsg_features = VMSG_FEATURES;
	resp->features = FFA_BUS_FEATURE_DIRECT_MSG_RX;
	resp->area_max_count = 0xff;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
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

	if (virtio_area_share(area_id, handle))
		resp->result = 1;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_ffa_bus_msg_area_unshare(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_ffa_bus_msg_area_unshare_req *req = (void *)(hdr + 1);
	struct virtio_msg_ffa_bus_msg_area_unshare_resp *resp = (void *)req;
	uint16_t area_id = req->area_id;
	TEE_Result res = TEE_SUCCESS;

	memset(resp, 0, sizeof(*resp));
	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req)) {
		hdr->dev_num = 0;
		goto out;
	}
	resp->area_id = area_id;

	res = virtio_area_unshare(area_id);
	if (res == TEE_ERROR_BUSY)
		resp->result = 2;
	else if (res)
		resp->result = 1;
out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_ffa_bus_msg_reset(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_ffa_bus_msg_reset_resp *resp = (void *)(hdr + 1);

	memset(resp, 0, sizeof(*resp));
	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr)) {
		hdr->dev_num = 0;
		resp->result = 1;
		goto out;
	}

	/*
	 * TODO
	 * For now fail until we have implemented the following:
	 * - unregister devices
	 * - shut down and free virtqueues
	 * - unshare areas
	 */
	resp->result = 1;

out:
	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void handle_ffa_bus_msg_event_configure(struct virtio_msg_hdr *hdr)
{
	struct virtio_msg_ffa_bus_msg_event_config_req *req = (void *)(hdr + 1);
	struct virtio_msg_ffa_bus_msg_event_config_resp *resp = (void *)req;

	memset(resp, 0, sizeof(*resp));
	if (hdr->dev_num || hdr->msg_size != sizeof(*hdr) + sizeof(*req))
		hdr->dev_num = 0;

	hdr->msg_size = sizeof(*hdr) + sizeof(*resp);
}

static void recv_bus_req(struct virtio_msg_hdr *hdr)
{
	switch (hdr->msg_id) {
	case FFA_BUS_MSG_VERSION:
		handle_ffa_bus_msg_version(hdr);
		break;
	case BUS_MSG_GET_DEVICES:
		virtio_bus_msg_handle_get_devices(hdr, FFA_MSG_SIZE);
		break;
	case BUS_MSG_PING:
		virtio_bus_msg_handle_ping(hdr);
		break;
	case FFA_BUS_MSG_AREA_SHARE:
		handle_ffa_bus_msg_area_share(hdr);
		break;
	case FFA_BUS_MSG_AREA_UNSHARE:
		handle_ffa_bus_msg_area_unshare(hdr);
		break;
	case FFA_BUS_MSG_RESET:
		handle_ffa_bus_msg_reset(hdr);
		break;
	case FFA_BUS_MSG_EVENT_CONFIGURE:
		handle_ffa_bus_msg_event_configure(hdr);
		break;
	default:
		assert(0);
		virtio_msg_set_null_msg(hdr);
	}
}

void virtio_msg_ffa_recv(struct thread_smc_1_2_regs *args)
{
	struct virtio_msg_hdr *hdr = (struct virtio_msg_hdr *)(args->a + 4);

	FMSG("hdr type %#x msg_id %#x dev_num %#x msg_size %#x",
	     hdr->type, hdr->msg_id, hdr->dev_num, hdr->msg_size);
	switch (hdr->type) {
	case MSG_TYPE_BUS_REQUEST:
		recv_bus_req(hdr);
		hdr->type = MSG_TYPE_BUS_RESPONSE;
		break;
	case MSG_TYPE_TRANSPORT_REQUEST:
		virtio_msg_recv_transport_req(hdr);
		hdr->type = MSG_TYPE_TRANSPORT_RESPONSE;
		break;
	default:
		assert(0);
		virtio_msg_set_null_msg(hdr);
	}
	assert(hdr->msg_size <= FFA_MSG_SIZE);
	memset((uint8_t *)hdr + hdr->msg_size, 0, FFA_MSG_SIZE - hdr->msg_size);
}
