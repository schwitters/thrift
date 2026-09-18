/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Transport public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_TRANSPORT_H
#define THRIFT_TRANSPORT_H
#include <thrift/c11/thrift.h>

/** @brief Borrowed exact-size I/O callbacks, copied into protocol objects.
 * Callbacks return THRIFT_OK only after transferring all requested bytes. On
 * failure partial transfer is possible and there is no automatic rollback.
 * The protocol never frees the context. Synchronize shared context access. */
struct thrift_transport {
    void *context; /**< Borrowed callback state, alive throughout all operations. */
    /** @brief Read exactly size bytes into data; required for decoding. Receives context, writable data
     * and size.
     */
    enum thrift_status (*read)(void *, void *, size_t);
    /** @brief Write exactly size bytes from data; required for encoding. Receives context, borrowed data
     * and size.
     */
    enum thrift_status (*write)(void *, const void *, size_t);
    /** @brief Optional synchronous output commit callback receiving context; NULL means no flush is
     * needed.
     */
    enum thrift_status (*flush)(void *);
};
#endif
