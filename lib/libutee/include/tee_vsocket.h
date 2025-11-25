/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2025, Linaro Limited
 */

#ifndef __TEE_VSOCKET_H
#define __TEE_VSOCKET_H

#include <tee_isocket.h>

/* Protocol identifier */
#define TEE_ISOCKET_PROTOCOLID_VSOCK	0x1b

/*
 * Instance specific ioctl functions and their arguments
 */

#define TEE_VSOCK_FLAG_SEQ_EOM	(1U << 0)
#define TEE_VSOCK_FLAG_SEQ_EOR	(1U << 1)

typedef enum TEE_vSocket_Type_e {
	TEE_VSOCKET_TYPE_STREAM,
	TEE_VSOCKET_TYPE_SEQPACKET,
} TEE_vSocket_Type;

typedef struct TEE_vSocket_Setup_s {
	TEE_vSocket_Type type;
	uint32_t port;
	bool listen;
} TEE_vSocket_Setup;

typedef struct TEE_vSocket_Recv_Flags_s {
	uint32_t timeout;
	uint32_t flags;
	void *buf;
	size_t buf_len;
} TEE_vSocket_Recv_Flags;
#define TEE_VSOCK_RECV_FLAGS	0x1bf00000

typedef struct TEE_vSocket_Send_Flags_s {
	uint32_t timeout;
	uint32_t flags;
	void *buf;
	size_t buf_len;
} TEE_vSocket_Send_Flags;
#define TEE_VSOCK_SEND_FLAGS	0x1bf00001

typedef struct TEE_vSocket_Get_Peer_s {
	uint64_t cid;
	uint32_t port;
} TEE_vSocket_Get_Peer;
#define TEE_VSOCK_GET_PEER	0x1bf00003

typedef struct TEE_VSocket_Accept_s {
	uint32_t timeout;
	TEE_iSocketHandle accept_ctx;
} TEE_VSocket_Accept;
#define TEE_VSOCK_ACCEPT	0x1bf00004

extern TEE_iSocket *const TEE_vSocket;

#endif /*__TEE_VSOCKET_H*/

