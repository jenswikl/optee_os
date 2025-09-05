/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2025, Linaro Limited
 */
#ifndef __KERNEL_VIRTIO_H
#define __KERNEL_VIRTIO_H

#include <bitstring.h>
#include <kernel/thread.h>
#include <tee_api_types.h>
#include <types_ext.h>

#define VIRTIO_F_VERSION_1			32

#define VIRTIO_F_ACCESS_PLATFORM		33
#define VIRTIO_MAX_FEATURE_BIT_COUNT		64

/* 2.1 Device Status Field */
#define VIRTIO_DEV_STATUS_ACKNOWLEDGE		BIT(0)
#define VIRTIO_DEV_STATUS_DRIVER		BIT(1)
#define VIRTIO_DEV_STATUS_FAILED		BIT(7)
#define VIRTIO_DEV_STATUS_FEATURES_OK		BIT(3)
#define VIRTIO_DEV_STATUS_DRIVER_OK		BIT(2)
#define VIRTIO_DEV_STATUS_DEVICE_NEEDS_RESET	BIT(6)

/* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT		1
/* This marks a buffer as write-only (otherwise read-only). */
#define VIRTQ_DESC_F_WRITE		2
/* This means the buffer contains a list of buffer descriptors. */
#define VIRTQ_DESC_F_INDIRECT		4
/*
 * The device uses this in used->flags to advise the driver: don't kick me
 * when you add a buffer. It's unreliable, so it's simply an optimization.
 */
#define VIRTQ_USED_F_NO_NOTIFY		1
/*
 * The driver uses this in avail->flags to advise the device: don't
 * interrupt me when you consume a buffer. It's unreliable, so it's simply
 * an optimization.
 */
#define VIRTQ_AVAIL_F_NO_INTERRUPT	1
/* Support for indirect descriptors */
#define VIRTIO_F_INDIRECT_DESC		28
/* Support for avail_event and used_event fields */
#define VIRTIO_F_EVENT_IDX		29
/* Arbitrary descriptor layouts. */
#define VIRTIO_F_ANY_LAYOUT		27

struct virtq_desc {
	uint64_t addr;	/* Buffer Address. */
	uint32_t len;	/* Buffer Length. */
	uint16_t flags;	/* The flags depending on descriptor type. */
	uint16_t next;	/* We chain unused descriptors via this, too */
};

struct virtq_used_elem {
	uint32_t id;	/* Index of start of used descriptor chain. */
	uint32_t len;	/* Total length of the descriptor chain which was */
			/* written to. */
};

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[];
	/* Only if VIRTIO_F_EVENT_IDX: le16 avail_event; */
};

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
	/* Only if VIRTIO_F_EVENT_IDX: le16 used_event; */
};

struct virtq {
	void (*callback)(struct virtq *vq);
	struct virtio_device *vdev;
	struct virtq_desc *desc;
	struct virtq_avail *driver;
	struct virtq_used *device;
	uint64_t desc_ba;
	uint64_t driver_ba;
	uint64_t device_ba;
	unsigned int desc_count;
	uint16_t avail_idx;
	unsigned int vq_id;
	bool enabled;
};

struct virtq_read_ctx {
	struct virtq *vq;
	uint16_t avail_idx;
	uint16_t first_desc_idx;
};

struct virtq_write_ctx {
	struct virtq *vq;
	uint16_t avail_idx;
	uint16_t first_desc_idx;
};

struct virtio_description {
	uint32_t dev_id;
	uint32_t vendor_id;
	uint8_t config_size;
	uint8_t vq_count;
	uint8_t admin_vq_start_idx;
	uint8_t admin_vq_count;
	uint8_t feature_bit_count;
	uint32_t max_desc_count;
	void (*get_features)(struct virtio_device *vdev, bitstr_t *bs,
			     size_t count, size_t offset);
	void (*set_features)(struct virtio_device *vdev, bitstr_t *bs,
			     size_t count, size_t offset);
	void (*get_config)(struct virtio_device *vdev, void *data,
			   size_t count, size_t offset);
	void (*set_virtque)(struct virtio_device *vdev, struct virtq *vq);
	struct virtq *(*get_virtque)(struct virtio_device *vdev, size_t vq_idx);
};

struct virtio_device {
	uint16_t dev_num;
	bitstr_t features[bitstr_size(VIRTIO_MAX_FEATURE_BIT_COUNT)];
	bool features_ok;
	uint8_t status;
	const struct virtio_description *desc;
	uint64_t conf_gen_count;
	struct virtq *vqs;
	TAILQ_ENTRY(virtio_device) link;
};

struct virtio_device *virtio_lookup_device(uint16_t dev_num);

TEE_Result virtio_register_device(struct virtio_device *vdev);
TEE_Result virtio_get_devices_bitstring(bitstr_t *bs, size_t count,
					size_t *pop_count);
TEE_Result virtio_get_features(uint16_t dev_num, bitstr_t *bs, size_t count);

TEE_Result virtq_init(struct virtq *vq, size_t vq_size, uint64_t vq_desc_addr,
		      uint64_t vq_drv_addr, uint64_t vq_dev_addr);
TEE_Result virtq_enable(struct virtq *vq);
void virtq_disable(struct virtq *vq);

TEE_Result virtq_read_start(struct virtq *vq, struct virtq_read_ctx *vqr);
TEE_Result virtq_read_copy(void *addr, struct virtq_read_ctx *vqr, size_t offs,
			   size_t len);
void virtq_read_finish(struct virtq_read_ctx *vqr);

TEE_Result virtq_write_start(struct virtq *vq, struct virtq_write_ctx *vqw,
			     size_t max_len);
TEE_Result virtq_write_copy(const void *addr, struct virtq_write_ctx *vqw,
			    size_t offs, size_t len);
void virtq_write_finish(struct virtq_write_ctx *vqw, size_t len);
void *virtio_bus_addr_to_virt(uint64_t bus_addr, size_t len);

#endif /*__KERNEL_VIRTIO_H*/
