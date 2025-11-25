/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2025, Linaro Limited
 */
#ifndef __KERNEL_VIRTIO_VSOCK_H
#define __KERNEL_VIRTIO_VSOCK_H

#include <sys/queue.h>
#include <tee_api_types.h>
#include <types_ext.h>

#define VIRTIO_VSOCK_TYPE_STREAM	1
#define VIRTIO_VSOCK_TYPE_SEQPACKET	2

#define VIRTIO_VSOCK_SEQ_EOM	BIT(0)
#define VIRTIO_VSOCK_SEQ_EOR	BIT(1)

TAILQ_HEAD(virtio_vsock_msg_head, virtio_vsock_msg);

struct virtio_vsock_socket {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint16_t type; /* VIRTIO_VSOCK_TYPE_* */
	bool listen;
	bool dead;
	uint16_t pending_op;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
	uint32_t rx_cnt;
	uint32_t peer_buf_alloc;
	uint32_t peer_fwd_cnt;
	uint32_t local_tx_cnt;
	void *owner;
	struct virtio_vsock_device *dev;
	TAILQ_ENTRY(virtio_vsock_socket) link;
	TAILQ_ENTRY(virtio_vsock_socket) link_backlog;
	struct virtio_vsock_msg_head msgs;
};

enum virtio_vsock_msg_type {
	VIRTIO_VSOCKET_MSG_TYPE_DATA,
	VIRTIO_VSOCKET_MSG_TYPE_REQ,
	VIRTIO_VSOCKET_MSG_TYPE_SHM,
};

struct virtio_vsock_msg {
	enum virtio_vsock_msg_type type;
	union {
		struct {
			uint32_t flags;
			void *buf;
			size_t len;
			size_t offs;
		} data;
		struct {
			uint64_t bus_addr;
			uint64_t len;
		} shm;
		struct virtio_vsock_socket *vvs_req;
	};
	TAILQ_ENTRY(virtio_vsock_msg) link;
};

TEE_Result virtio_vsock_listen(uint32_t port, uint16_t type, void *owner,
			       struct virtio_vsock_socket **vvs);
void virtio_vsock_close(struct virtio_vsock_socket *vvs);
/* Convenience function for handle_db_destroy() */
static inline void virtio_vsock_destructor(void *ptr)
{
	virtio_vsock_close(ptr);
}

void virtio_vsock_msgq_lock(struct virtio_vsock_socket *vvs);
void virtio_vsock_msgq_unlock(struct virtio_vsock_socket *vvs);

/* Returns pointer to head, doesn't remove, assumes lock held */
struct virtio_vsock_msg *
virtio_vsock_msgq_peek(struct virtio_vsock_socket *vvs,
		       enum virtio_vsock_msg_type type, uint32_t timeout);

/* Removes head, assumes lock held */
void virtio_vsock_msgq_dequeue(struct virtio_vsock_socket *vvs,
			       struct virtio_vsock_msg *msg);

TEE_Result virtio_vsock_send(struct virtio_vsock_socket *vvs, const void *buf,
			     size_t *blen, uint32_t flags, uint32_t timeout);

#endif /*__KERNEL_VIRTIO_VSOCK_H*/
