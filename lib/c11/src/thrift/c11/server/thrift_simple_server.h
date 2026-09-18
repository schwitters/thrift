/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Simple server public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_SIMPLE_SERVER_H
#define THRIFT_SIMPLE_SERVER_H
#include <thrift/c11/processor/thrift_processor.h>
#include <thrift/c11/transport/thrift_socket.h>

/**
 * @brief Accept one connection, process requests and close it.
 *
 * Uses versioned Binary. Accept itself blocks. Processing stops at the first error.
 * The accepted socket is closed on all paths; listener and handler stay borrowed.
 * The processor must not retain the temporary protocol or accepted connection.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] listener Borrowed live listening socket.
 * @param[in] process Non-NULL synchronous processor callback.
 * @param[in,out] handler Borrowed non-NULL callback context.
 * @param[in] request_count Positive number of requests to process on one connection.
 * @param[in] timeout_ms Positive timeout for each blocking read/write, in milliseconds.
 * @return THRIFT_OK if all requests and close succeed; otherwise the first setup/processing error, or the close error.
 */
enum thrift_status thrift_server_serve(
    struct thrift_socket *listener,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, size_t request_count, uint32_t timeout_ms);

/**
 * @brief Accept one connection, process requests and close it.
 *
 * Uses the specified protocol. Accept itself blocks. Processing stops at the first error.
 * The accepted socket is closed on all paths; listener and handler stay borrowed.
 * The processor must not retain the temporary protocol or accepted connection.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] listener Borrowed live listening socket.
 * @param[in] process Non-NULL synchronous processor callback.
 * @param[in,out] handler Borrowed non-NULL callback context.
 * @param[in] request_count Positive number of requests to process on one connection.
 * @param[in] timeout_ms Positive timeout for each blocking read/write, in milliseconds.
 * @param[in] kind THRIFT_BINARY or THRIFT_COMPACT.
 * @return THRIFT_OK if all requests and close succeed; otherwise the first setup/processing error, or the close error.
 */
enum thrift_status thrift_server_serve_kind(
    struct thrift_socket *listener,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, size_t request_count, uint32_t timeout_ms,
    enum thrift_protocol_kind kind);
#endif
