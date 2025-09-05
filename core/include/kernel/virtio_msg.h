/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2025, Linaro Limited
 */
#ifndef __KERNEL_VIRTIO_MSG_H
#define __KERNEL_VIRTIO_MSG_H

#include <stdint.h>

#define BUS_MSG_GET_DEVICES		0x02
#define BUS_MSG_PING			0x03
#define BUS_MSG_EVENT_DEVICE		0x40

#define MSG_TYPE_TRANSPORT_REQUEST	0x0
#define MSG_TYPE_TRANSPORT_RESPONSE	0x1
#define MSG_TYPE_BUS_REQUEST		0x2
#define MSG_TYPE_BUS_RESPONSE		0x3

struct virtio_msg_hdr {
	uint8_t type;
	uint8_t msg_id;
	uint16_t dev_num;
	uint16_t msg_uid;
	uint16_t msg_size;
};

static void virtio_msg_set_null_msg(struct virtio_msg_hdr *hdr)
{
	*hdr = (struct virtio_msg_hdr){
		.type = MSG_TYPE_BUS_RESPONSE,
		.msg_size = 6,
	};
}

void virtio_bus_msg_handle_get_devices(struct virtio_msg_hdr *hdr,
				       size_t max_msg_size);
void virtio_bus_msg_handle_ping(struct virtio_msg_hdr *hdr);
void virtio_msg_recv_transport_req(struct virtio_msg_hdr *hdr);

#endif /*__KERNEL_VIRTIO_MSG_H*/

