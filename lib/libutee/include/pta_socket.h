/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2016, 2025 Linaro Limited
 */

#ifndef __PTA_SOCKET
#define __PTA_SOCKET

#define PTA_SOCKET_UUID { 0x3b996a7d, 0x2c2b, 0x4a49, { \
			  0xa8, 0x96, 0xe1, 0xfb, 0x57, 0x66, 0xd2, 0xf4 } }

/*
 * [in]		value[0].a	ip version TEE_IP_VERSION_* from tee_ipsocket.h
 * [in]		value[0].b	server port number
 * [in]		memref[1]	server address
 * [in]		value[2].a	protocol, TEE_ISOCKET_PROTOCOLID_*
 * [out]	value[3].a	socket handle
 */
#define PTA_SOCKET_OPEN		1

/*
 * [in]		value[0].a	socket handle
 */
#define PTA_SOCKET_CLOSE	2

#define PTA_SOCKET_TIMEOUT_NONBLOCKING	0
#define PTA_SOCKET_TIMEOUT_BLOCKING	0xffffffff

/*
 * [in]		value[0].a	socket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [in]		memref[1]	buffer to transmit
 * [out]	value[2].a	number of transmitted bytes
 */
#define PTA_SOCKET_SEND		3

/*
 * [in]		value[0].a	socket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [out]	memref[1]	buffer
 */
#define PTA_SOCKET_RECV		4

/*
 * [in]		value[0].a	socket handle
 * [in]		value[0].b	ioctl command
 * [in/out]	memref[1]	buffer
 */
#define PTA_SOCKET_IOCTL	5

/*
 * [in]		value[0].a	vsocket type TEE_VSOCKET_TYPE_*
 * [in]		value[0].b	port number
 * [out]	value[1].a	socket handle
 */
#define PTA_SOCKET_VSOCK_OPEN		6

/*
 * [in]		value[0].a	vsocket handle
 */
#define PTA_SOCKET_VSOCK_CLOSE		7

/*
 * [in]		value[0].a	socket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [out]	memref[1]	buffer
 */
#define PTA_SOCKET_VSOCK_RECV		8

/*
 * [in]		value[0].a	vsocket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [out]	memref[1]	buffer
 * [out]	value[2].a	flags, TEE_VSOCK_FLAG_*
 */
#define PTA_SOCKET_VSOCK_RECV_FLAGS	9

/*
 * [in]		value[0].a	vsocket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [in]		memref[1]	buffer
 * [in]		value[2].a	flags, TEE_VSOCK_FLAG_*
 * [out]	value[3].a	sent bytes
 */
#define PTA_SOCKET_VSOCK_SEND_FLAGS	10

/*
 * [in]		value[0].a	vsocket handle
 * [out]	value[1].a	cid upper 32-bits
 * [out]	value[1].b	cid lower 32-bits
 * [out]	value[2].a	port
 */
#define PTA_SOCKET_VSOCK_GET_PEER	11

/*
 * [in]		value[0].a	vsocket handle
 * [in]		value[0].b	timeout ms or TEE_TIMEOUT_INFINITE
 * [out]	value[1].a	acceted socket handle
 */
#define PTA_SOCKET_VSOCK_ACCEPT		12

#endif /*__PTA_SOCKET*/
