/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Http server transport public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_HTTP_SERVER_TRANSPORT_H
#define THRIFT_HTTP_SERVER_TRANSPORT_H
#include <thrift/c11/transport/thrift_transport.h>

struct mg_connection;
/** @brief Opaque request-scoped transport borrowing a CivetWeb connection. */
struct thrift_http_server_transport;

/**
 * @brief Read one complete HTTP request into a bounded transport.
 *
 * Supports Content-Length and chunked bodies. Writes/flush buffer the reply; only
 * finish sends it. The caller handles HTTP errors if creation fails.
 * @note Call only inside a CivetWeb request handler with exclusive connection access.
 *
 * @param[in,out] conn Borrowed CivetWeb connection, valid throughout this request handler.
 * @param[in] max_body_size Positive independent request/response byte bound, at most LLONG_MAX.
 * @param[out] result Owned request transport; cleared to NULL when both outputs are valid.
 * @param[out] transport Borrowed view of the new request transport; assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, THRIFT_LIMIT, THRIFT_EOF, or THRIFT_IO.
 */
enum thrift_status thrift_http_server_transport_create(struct mg_connection *conn,
    size_t max_body_size, struct thrift_http_server_transport **result,
    struct thrift_transport *transport);
/**
 * @brief Commit one HTTP response after processing completes.
 *
 * Rejects unread trailing request bytes before committing the RPC response. Success
 * sends HTTP 200 (empty for oneway); failure sends an HTTP error without a partial
 * RPC body. Processing may already have changed application state. Sending itself
 * can fail partway through. The handle becomes finished even on failure.
 * @note Exclusive access inside the originating request handler is required.
 *
 * @param[in,out] transport Live request transport not previously finished.
 * @param[in] process_status Final status from the RPC processor.
 * @return THRIFT_OK on success; the processing/buffering error, THRIFT_PROTOCOL for trailing request bytes, THRIFT_INVALID for repeated finish, or THRIFT_IO if sending fails.
 */
enum thrift_status thrift_http_server_transport_finish(
    struct thrift_http_server_transport *transport, enum thrift_status process_status);
/**
 * @brief Release request/response buffers without closing the borrowed connection.
 *
 * Does not call finish or send a response. Invalidates borrowed transport views.
 * @note Exclusive access inside the originating request handler is required.
 *
 * @param[in,out] transport Owned request transport; NULL is a no-op.
 */
void thrift_http_server_transport_destroy(struct thrift_http_server_transport *transport);
#endif
