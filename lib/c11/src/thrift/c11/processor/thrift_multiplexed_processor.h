/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Multiplexed processor public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_MULTIPLEXED_PROCESSOR_H
#define THRIFT_MULTIPLEXED_PROCESSOR_H
#include <thrift/c11/processor/thrift_processor.h>

/**
 * @brief Dispatch one request using its service:method name.
 *
 * Names must be unique, nonempty and contain no colon. Registrations, names and
 * handlers must live throughout the call. Unknown services/methods produce an
 * application exception, or no reply for oneway.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Exclusively held Binary or Compact protocol.
 * @param[in] services Borrowed service registrations; non-NULL when count is nonzero.
 * @param[in] count Number of registrations.
 * @param[in] default_service NULL to reject unprefixed calls, or a registered service name.
 * @return THRIFT_OK after successful handling/reply; THRIFT_INVALID for invalid registrations, or another runtime error.
 */
enum thrift_status thrift_multiplexed_process(struct thrift_protocol *protocol,
    const struct thrift_service *services, size_t count, const char *default_service);
#endif
