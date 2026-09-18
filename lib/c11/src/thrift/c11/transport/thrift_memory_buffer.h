/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Memory buffer public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_MEMORY_BUFFER_H
#define THRIFT_MEMORY_BUFFER_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Caller-owned fixed-capacity memory transport state.
 * Keep 0 <= position <= size <= capacity; storage must outlive the transport.
 * Reads consume at position; writes append at size. Requires exclusive access. */
struct thrift_memory {
    uint8_t *data; /**< Borrowed writable backing storage. */
    size_t capacity; /**< Total storage capacity in bytes. */
    size_t size; /**< Valid byte count and next write offset. */
    size_t position; /**< Next read offset. */
};
/**
 * @brief Bind a fixed-capacity caller-owned buffer to a transport.
 *
 * No allocation occurs. Reads advance position and return THRIFT_EOF if insufficient
 * bytes remain. Writes append at size and return THRIFT_LIMIT if capacity is exceeded.
 * Those failed operations do not advance position/size. No flush callback is needed.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[out] memory Caller-owned state; must outlive transport use.
 * @param[in] data Borrowed writable buffer; NULL is allowed only with zero capacity.
 * @param[in] capacity Buffer capacity in bytes.
 * @param[in] read_size Initial valid byte count, no greater than capacity.
 * @param[out] transport Transport view borrowing memory.
 * @return THRIFT_OK, or THRIFT_INVALID; outputs are unchanged on invalid arguments.
 */
enum thrift_status thrift_memory_init(struct thrift_memory *memory,
                                             void *data, size_t capacity, size_t read_size,
                                             struct thrift_transport *transport);

#endif
