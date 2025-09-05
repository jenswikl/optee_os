// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited.
 */

#include <initcall.h>
#include <kernel/notif.h>
#include <kernel/thread_spmc.h>
#include <kernel/virtio.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <mm/mobj.h>
#include <string.h>
#include <sys/queue.h>

#define VIRTIO_MSG_FFA_AREA_ID_MASK	0xFFFF
#define VIRTIO_MSG_FFA_AREA_ID_SHIFT	48

TAILQ_HEAD(virtio_device_head, virtio_device);

struct virtio_area {
	struct mobj *mobj;
	uint64_t handle;
	uint16_t id;
};

static struct virtio_area virtio_area;

static uint16_t next_dev_num;
static struct virtio_device_head virtio_device_head =
	TAILQ_HEAD_INITIALIZER(virtio_device_head);

TEE_Result virtio_get_devices_bitstring(bitstr_t *bs, size_t count,
					size_t *pop_count)
{
	struct virtio_device *vdev = NULL;
	size_t c = 0;

	memset(bs, 0, bitstr_size(count));
	TAILQ_FOREACH(vdev, &virtio_device_head, link) {
		if (vdev->dev_num >= count)
			return TEE_ERROR_SHORT_BUFFER;

		bit_set(bs, vdev->dev_num);
		c++;
	}

	*pop_count = c;

	return TEE_SUCCESS;
}

struct virtio_device *virtio_lookup_device(uint16_t dev_num)
{
	struct virtio_device *vdev = NULL;

	TAILQ_FOREACH(vdev, &virtio_device_head, link)
		if (vdev->dev_num == dev_num)
			return vdev;

	return NULL;
}

TEE_Result virtio_register_device(struct virtio_device *vdev)
{
	size_t n = 0;

	if (next_dev_num == UINT16_MAX)
		return TEE_ERROR_GENERIC;

	vdev->vqs = calloc(vdev->desc->vq_count, sizeof(*vdev->vqs));
	if (!vdev->vqs)
		return TEE_ERROR_OUT_OF_MEMORY;
	for (n = 0; n < vdev->desc->vq_count; n++)
		vdev->vqs[n].idx = n;

	vdev->dev_num = next_dev_num;
	next_dev_num++;

	TAILQ_INSERT_TAIL(&virtio_device_head, vdev, link);

	return TEE_SUCCESS;
}

TEE_Result virtq_init(struct virtq *vq, size_t vq_size, uint64_t vq_desc_addr,
		     uint64_t vq_drv_addr, uint64_t vq_dev_addr)
{
	size_t sz = 0;

	DMSG("vq idx %u size %zu desc %#"PRIx64" drv %#"PRIx64" dev %#"PRIx64,
	     vq->idx, vq_size, vq_desc_addr, vq_drv_addr, vq_dev_addr);
	if (MUL_OVERFLOW(vq_size, sizeof(*vq->desc), &sz))
		return TEE_ERROR_BAD_PARAMETERS;

	vq->desc = virtio_bus_addr_to_virt(vq_desc_addr, sz);
	if (!vq->desc)
		return TEE_ERROR_BAD_PARAMETERS;
	vq->driver = virtio_bus_addr_to_virt(vq_drv_addr, sizeof(*vq->driver));
	if (!vq->driver)
		return TEE_ERROR_BAD_PARAMETERS;
	vq->device = virtio_bus_addr_to_virt(vq_dev_addr, sizeof(*vq->device));
	if (!vq->device)
		return TEE_ERROR_BAD_PARAMETERS;

	DMSG("vq idx %u PA desc %#"PRIxPA" drv %#"PRIxPA" dev %#"PRIxPA,
	     vq->idx, virt_to_phys(vq->desc), virt_to_phys(vq->driver),
	     virt_to_phys(vq->device));

	vq->desc_count = vq_size;
	return TEE_SUCCESS;
}

TEE_Result virtq_enable(struct virtq *vq)
{
	assert(!vq->enabled);
	vq->enabled = true;
	return TEE_SUCCESS;
}

void virtq_disable(struct virtq *vq)
{
	assert(vq->enabled);
	vq->enabled = false;
}

TEE_Result virtio_area_share_ffa(uint16_t area_id, uint64_t handle)
{
	struct mobj *m = NULL;

	if (virtio_area.mobj)
		return TEE_ERROR_BUSY; // TOOD support more areas

	m = mobj_ffa_get_by_cookie(handle, 0);
	if (!m)
		return TEE_ERROR_GENERIC; // TODO handle attr + tag
	if (mobj_inc_map(m)) { // TODO handle per lookup
		mobj_ffa_unregister_by_cookie(handle);
		return TEE_ERROR_GENERIC;
	}

	virtio_area.mobj = m;
	virtio_area.handle = handle;
	virtio_area.id = area_id;
	return TEE_SUCCESS;
}

TEE_Result virtio_area_unshare(uint16_t area_id)
{
	TEE_Result res = TEE_SUCCESS;

	if (virtio_area.mobj || virtio_area.id != area_id)
		return TEE_ERROR_ITEM_NOT_FOUND;

	if (mobj_dec_map(virtio_area.mobj))
		return TEE_ERROR_GENERIC;
	res = mobj_ffa_unregister_by_cookie(virtio_area.handle);
	if (!res)
		memset(&virtio_area, 0, sizeof(virtio_area));

	return res;
}

void *virtio_bus_addr_to_virt(uint64_t bus_addr, size_t len)
{
	uint16_t area_id = 0;
	size_t offs = 0;

	area_id = (bus_addr >> VIRTIO_MSG_FFA_AREA_ID_SHIFT) &
		  VIRTIO_MSG_FFA_AREA_ID_MASK;
	offs = bus_addr & ~SHIFT_U64(VIRTIO_MSG_FFA_AREA_ID_MASK,
				     VIRTIO_MSG_FFA_AREA_ID_SHIFT);

	if (virtio_area.id != area_id)
		return NULL;

	return mobj_get_va(virtio_area.mobj, offs, len);
}

TEE_Result virtq_payload_read_init(struct virtq *vq, struct virtq_rbuf *vqr)
{
	if (vq->avail_idx == vq->driver->idx)
		return TEE_ERROR_ITEM_NOT_FOUND;

	DMSG("vq id %u vq->avail_idx %u vq->driver->idx %u",
	     vq->idx, vq->avail_idx, vq->driver->idx);
	*vqr = (struct virtq_rbuf){
		.vq = vq,
		.avail_idx = vq->avail_idx,
		.first_desc_idx = vq->driver->ring[vq->avail_idx %
						   vq->desc_count],
	};

	return TEE_SUCCESS;
}

TEE_Result virtq_copy_from_rbuf(void *addr, struct virtq_rbuf *vqr,
				size_t offs, size_t len)
{
	size_t desc_idx = vqr->first_desc_idx;
	struct virtq *vq = vqr->vq;
	struct virtq_desc *desc = vq->desc;
	size_t dst_offs = 0;
	void *src = NULL;
	uint64_t bus_addr = 0;
	size_t l = 0;
	size_t o = 0;

	while (true) {
		if (desc_idx >= vq->desc_count)
			return TEE_ERROR_GENERIC;
		if (desc[desc_idx].flags & VIRTQ_DESC_F_WRITE)
			return TEE_ERROR_GENERIC;

		if (desc[desc_idx].len + o > offs) {
			if (o < offs) {
				bus_addr = desc[desc_idx].addr + offs - o;
				l = desc[desc_idx].len + offs - o;
			} else {
				bus_addr = desc[desc_idx].addr;
				l = desc[desc_idx].len;
			}
			l = MIN(l, len - dst_offs);
			src = virtio_bus_addr_to_virt(bus_addr, l);
			DMSG("bus_addr %#"PRIx64" VA %p PA %#"PRIxPA,
			     bus_addr, src, virt_to_phys(src));
			if (!src)
				return TEE_ERROR_GENERIC;
			memcpy((uint8_t *)addr + dst_offs, src, l);

			dst_offs += l;
			assert(dst_offs <= len);
			if (dst_offs == len)
				return TEE_SUCCESS;
		}

		o += desc[desc_idx].len;
		if (desc[desc_idx].flags & VIRTQ_DESC_F_NEXT)
			desc_idx = desc[desc_idx].next;
		else
			return TEE_ERROR_GENERIC;
	}
}

void virtq_rbuf_consume(struct virtq_rbuf *vqr)
{
	struct virtq *vq = vqr->vq;
	struct virtq_used *used = vq->device;
	size_t ring_idx = used->idx % vq->desc_count;

	DMSG("used->ring[%zu] PA %#"PRIxPA" id %"PRIu16,
		ring_idx, virt_to_phys(used), vqr->first_desc_idx);
	used->ring[ring_idx].id = vqr->first_desc_idx;
	used->ring[ring_idx].len = 0;
	dsb();
	vq->avail_idx = vqr->avail_idx + 1;
	used->idx = vq->avail_idx;

	vqr->vq = NULL;
}

TEE_Result virtq_payload_write_init(struct virtq *vq, struct virtq_wbuf *vqw,
				    size_t max_len __unused)
{
	if (vq->avail_idx == vq->driver->idx)
		return TEE_ERROR_ITEM_NOT_FOUND;

	DMSG("vq id %u vq->avail_idx %u vq->driver->idx %u",
	     vq->idx, vq->avail_idx, vq->driver->idx);
	*vqw = (struct virtq_wbuf){
		.vq = vq,
		.avail_idx = vq->avail_idx,
		.first_desc_idx = vq->driver->ring[vq->avail_idx %
						   vq->desc_count],
	};

	// TODO check that there's room for max_len

	return TEE_SUCCESS;
}

TEE_Result virtq_copy_to_wbuf(void *addr, struct virtq_wbuf *vqw,
				size_t offs, size_t len)
{
	size_t desc_idx = vqw->first_desc_idx;
	struct virtq *vq = vqw->vq;
	struct virtq_desc *desc = vq->desc;
	size_t src_offs = 0;
	void *dst = NULL;
	uint64_t bus_addr = 0;
	size_t l = 0;
	size_t o = 0;

	while (true) {
		if (desc_idx >= vq->desc_count)
			return TEE_ERROR_GENERIC;
		if (!(desc[desc_idx].flags & VIRTQ_DESC_F_WRITE))
			return TEE_ERROR_GENERIC;
		if (desc[desc_idx].len + o > offs) {
			if (o < offs) {
				bus_addr = desc[desc_idx].addr + offs - o;
				l = desc[desc_idx].len + offs - o;
			} else {
				bus_addr = desc[desc_idx].addr;
				l = desc[desc_idx].len;
			}
			l = MIN(l, len - src_offs);
			dst = virtio_bus_addr_to_virt(bus_addr, l);
			DMSG("bus_addr %#"PRIx64" VA %p PA %#"PRIxPA,
			     bus_addr, dst, virt_to_phys(dst));
			if (!dst)
				return TEE_ERROR_GENERIC;
			memcpy(dst, (uint8_t *)addr + src_offs, l);

			src_offs += l;
			assert(src_offs <= len);
			if (src_offs == len)
				return TEE_SUCCESS;
		}

		o += desc[desc_idx].len;
		if (desc[desc_idx].flags & VIRTQ_DESC_F_NEXT)
			desc_idx = desc[desc_idx].next;
		else
			return TEE_ERROR_GENERIC;

	}
}

void virtq_wbuf_deliver(struct virtq_wbuf *vqw, size_t len)
{
	struct virtq *vq = vqw->vq;
	struct virtq_used *used = vq->device;
	size_t ring_idx = used->idx % vq->desc_count;

	DMSG("used->ring[%zu] PA %#"PRIxPA" id %"PRIu16" len %zu",
		ring_idx, virt_to_phys(used), vqw->first_desc_idx, len);
	used->ring[ring_idx].id = vqw->first_desc_idx;
	used->ring[ring_idx].len = len;
	dsb();
	vq->avail_idx = vqw->avail_idx + 1;
	used->idx = vq->avail_idx;

	vqw->vq = NULL;
}

static void atomic_virtq_notif(struct notif_driver *ndrv __unused,
			       enum notif_event ev __maybe_unused,
			       uint16_t vm_id __maybe_unused)
{
	DMSG("Event %d vm_id %#"PRIx16, (int)ev, vm_id);
}

static void yielding_virtq_notif(struct notif_driver *ndrv __unused,
				 enum notif_event ev)
{
	struct virtio_device *vdev = NULL;
	struct virtq *vq = NULL;

	DMSG("Event %d", (int)ev);
	if (ev != NOTIF_EVENT_DO_BOTTOM_HALF)
		return;

	// TODO lock
	TAILQ_FOREACH(vdev, &virtio_device_head, link) {
		// XXX knows that only one vq should be processed
		assert(vdev->vqs && vdev->desc->vq_count == 3);
		vq = vdev->vqs + 1;
		if (vq->enabled) {
			assert(vq->callback);
			vq->callback(vq);
		}
	}
}

static struct notif_driver virtq_notif __nex_data = {
	.atomic_cb = atomic_virtq_notif,
	.yielding_cb = yielding_virtq_notif,
};

static TEE_Result init_virtq_processing(void)
{
	notif_register_driver(&virtq_notif);

	return TEE_SUCCESS;
}
nex_service_init(init_virtq_processing);
