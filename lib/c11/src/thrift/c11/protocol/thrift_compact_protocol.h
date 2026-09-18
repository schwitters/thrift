/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Compact protocol public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_COMPACT_PROTOCOL_H
#define THRIFT_COMPACT_PROTOCOL_H
#include <thrift/c11/protocol/thrift_protocol.h>

/**
 * @brief Initialize Compact version 1.
 *
 * At least one of the read/write callbacks must be present. The backing transport
 * context must outlive all uses of the protocol. Installs default limits: 16 MiB
 * wire bytes, 32 MiB allocation, 1048576 container elements and depth 64.
 * No I/O or allocation is performed.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[out] protocol Caller-owned protocol storage.
 * @param[in] transport Transport view copied by value; its context remains borrowed.
 * @return THRIFT_OK, or THRIFT_INVALID for invalid arguments.
 */
enum thrift_status thrift_compact_protocol_init(struct thrift_protocol *protocol,
                                                       struct thrift_transport transport);
#endif
