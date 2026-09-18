/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Multiplexed protocol public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_MULTIPLEXED_PROTOCOL_H
#define THRIFT_MULTIPLEXED_PROTOCOL_H
#include <thrift/c11/protocol/thrift_protocol.h>

/**
 * @brief Initialize a client protocol with multiplexed request names.
 *
 * CALL/ONEWAY names become service:method; replies retain the method name.
 * Uses the same default limits as thrift_protocol_init_kind().
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[out] protocol Caller-owned protocol storage.
 * @param[in] transport Copied transport view with borrowed context.
 * @param[in] kind THRIFT_BINARY or THRIFT_COMPACT.
 * @param[in] service_name Nonempty borrowed string without a colon; must outlive protocol use.
 * @return THRIFT_OK, or THRIFT_INVALID for invalid arguments.
 */
enum thrift_status thrift_multiplexed_protocol_init(
    struct thrift_protocol *protocol, struct thrift_transport transport,
    enum thrift_protocol_kind kind, const char *service_name);
#endif
