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

TAILQ_HEAD(virtio_device_head, virtio_device);

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
		vdev->vqs[n].vq_id = n;

	vdev->dev_num = next_dev_num;
	next_dev_num++;

	TAILQ_INSERT_TAIL(&virtio_device_head, vdev, link);

	return TEE_SUCCESS;
}

TEE_Result virtq_init(struct virtq *vq, size_t vq_size, uint64_t vq_desc_addr,
		      uint64_t vq_drv_addr, uint64_t vq_dev_addr)
{
	TEE_Result res = TEE_SUCCESS;
	size_t sz = 0;

	DMSG("vq idx %u size %zu desc %#"PRIx64" drv %#"PRIx64" dev %#"PRIx64,
	     vq->vq_id, vq_size, vq_desc_addr, vq_drv_addr, vq_dev_addr);
	if (MUL_OVERFLOW(vq_size, sizeof(*vq->desc), &sz))
		return TEE_ERROR_BAD_PARAMETERS;

	res = virtio_bus_addr_inc_map(vq_desc_addr);
	if (res)
		return res;
	vq->desc = virtio_bus_addr_to_virt(vq_desc_addr, sz);
	if (!vq->desc) {
		res = TEE_ERROR_BAD_PARAMETERS;
		goto err_desc;
	}

	res = virtio_bus_addr_inc_map(vq_drv_addr);
	if (res)
		goto err_desc;
	vq->driver = virtio_bus_addr_to_virt(vq_drv_addr, sizeof(*vq->driver));
	if (!vq->driver) {
		goto err_drv;
		res = TEE_ERROR_BAD_PARAMETERS;
	}

	res = virtio_bus_addr_inc_map(vq_dev_addr);
	if (res)
		goto err_drv;
	vq->device = virtio_bus_addr_to_virt(vq_dev_addr, sizeof(*vq->device));
	if (!vq->device) {
		goto err_dev;
		res = TEE_ERROR_BAD_PARAMETERS;
	}

	DMSG("vq idx %u PA desc %#"PRIxPA" drv %#"PRIxPA" dev %#"PRIxPA,
	     vq->vq_id, virt_to_phys(vq->desc), virt_to_phys(vq->driver),
	     virt_to_phys(vq->device));

	vq->desc_ba = vq_desc_addr;
	vq->driver_ba = vq_drv_addr;
	vq->device_ba = vq_dev_addr;
	vq->desc_count = vq_size;
	return TEE_SUCCESS;

err_dev:
	virtio_bus_addr_dec_map(vq_dev_addr);
err_drv:
	virtio_bus_addr_dec_map(vq_drv_addr);
err_desc:
	virtio_bus_addr_dec_map(vq_desc_addr);
	return res;
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

TEE_Result virtq_read_start(struct virtq *vq, struct virtq_read_ctx *vqr)
{
	if (vq->avail_idx == vq->driver->idx)
		return TEE_ERROR_ITEM_NOT_FOUND;

	FMSG("vq id %u vq->avail_idx %u vq->driver->idx %u",
	     vq->vq_id, vq->avail_idx, vq->driver->idx);
	*vqr = (struct virtq_read_ctx){
		.vq = vq,
		.avail_idx = vq->avail_idx,
		.first_desc_idx = vq->driver->ring[vq->avail_idx %
						   vq->desc_count],
	};

	return TEE_SUCCESS;
}

TEE_Result virtq_read_copy(void *addr, struct virtq_read_ctx *vqr, size_t offs,
			   size_t len)
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
			FMSG("bus_addr %#"PRIx64" VA %p PA %#"PRIxPA,
			     bus_addr, src, virt_to_phys(src));
			if (!src)
				return TEE_ERROR_GENERIC;
			if (TRACE_LEVEL >= TRACE_FLOW)
				DHEXDUMP(src, len);
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

void virtq_read_finish(struct virtq_read_ctx *vqr)
{
	struct virtq *vq = vqr->vq;
	struct virtq_used *used = vq->device;
	size_t ring_idx = used->idx % vq->desc_count;

	FMSG("used->ring[%zu] PA %#"PRIxPA" id %"PRIu16,
	     ring_idx, virt_to_phys(used), vqr->first_desc_idx);
	used->ring[ring_idx].id = vqr->first_desc_idx;
	used->ring[ring_idx].len = 0;
	dsb();
	vq->avail_idx = vqr->avail_idx + 1;
	used->idx = vq->avail_idx;

	vqr->vq = NULL;
}

TEE_Result virtq_write_start(struct virtq *vq, struct virtq_write_ctx *vqw,
			     size_t max_len __unused)
{
	if (vq->avail_idx == vq->driver->idx)
		return TEE_ERROR_ITEM_NOT_FOUND;

	FMSG("vq id %u vq->avail_idx %u vq->driver->idx %u",
	     vq->vq_id, vq->avail_idx, vq->driver->idx);
	*vqw = (struct virtq_write_ctx){
		.vq = vq,
		.avail_idx = vq->avail_idx,
		.first_desc_idx = vq->driver->ring[vq->avail_idx %
						   vq->desc_count],
	};

	/*
	 * TODO determine if we should check that there's room for max_len
	 * or if we should let virtq_write_copy() fail if there isn't.
	 */

	return TEE_SUCCESS;
}

TEE_Result virtq_write_copy(const void *addr, struct virtq_write_ctx *vqw,
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
			FMSG("bus_addr %#"PRIx64" VA %p PA %#"PRIxPA,
			     bus_addr, dst, virt_to_phys(dst));
			if (!dst)
				return TEE_ERROR_GENERIC;
			memcpy(dst, (const uint8_t *)addr + src_offs, l);

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

void virtq_write_finish(struct virtq_write_ctx *vqw, size_t len)
{
	struct virtq *vq = vqw->vq;
	struct virtq_used *used = vq->device;
	size_t ring_idx = used->idx % vq->desc_count;

	FMSG("used->ring[%zu] PA %#"PRIxPA" id %"PRIu16" len %zu",
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
	size_t n = 0;

	DMSG("Event %d", (int)ev);
	if (ev != NOTIF_EVENT_DO_BOTTOM_HALF)
		return;

	/*
	 * Unlocked since we only modify this during boot while we're still
	 * single threaded. This will need to change once we add or remove
	 * devices after boot.
	 */
	TAILQ_FOREACH(vdev, &virtio_device_head, link) {
		for (n = 0; n < vdev->desc->vq_count; n++) {
			vq = vdev->vqs + n;
			if (vq->enabled)
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
