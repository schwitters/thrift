/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Zstd transport public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_ZSTD_TRANSPORT_H
#define THRIFT_ZSTD_TRANSPORT_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Opaque owned Zstandard wrapper; exclusive access is required per instance. */
struct thrift_zstd;

/**
 * @brief Wrap a byte transport with bounded Zstandard frames.
 *
 * Each flush emits one standard frame (also for empty output) and flushes the
 * underlying transport if supported. Reads validate a whole frame/checksum before
 * exposing bytes. Dictionaries and skippable frames are unsupported. This framing
 * is not the C++ Zlib transport format. Discard wrapper and connection on I/O/codec
 * errors. The underlying context must outlive the wrapper.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] underlying Borrowed transport with both read and write callbacks; flush is optional.
 * @param[in] max_frame_size Positive bound on uncompressed bytes per frame; compressed input is also bounded.
 * @param[in] compression_level Level accepted by the linked libzstd, including zero for its default.
 * @param[out] result Owned wrapper; cleared to NULL when both outputs are valid.
 * @param[out] transport Borrowed view of the wrapper, assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT, or THRIFT_NOMEM.
 */
enum thrift_status thrift_zstd_create(struct thrift_transport underlying,
    size_t max_frame_size, int compression_level, struct thrift_zstd **result,
    struct thrift_transport *transport);
/**
 * @brief Release compression buffers without flushing or closing the underlying transport.
 *
 * Invalidates all borrowed views of the wrapper.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] zstd Owned wrapper; NULL is a no-op.
 */
void thrift_zstd_destroy(struct thrift_zstd *zstd);
#endif
