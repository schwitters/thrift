/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief CivetWeb server for binary WebSocket Thrift messages.
 */
#ifndef THRIFT_WEBSOCKET_SERVER_H
#define THRIFT_WEBSOCKET_SERVER_H
#include <thrift/c11/protocol/thrift_protocol.h>
/** @brief Owned listener and connection workers. */
struct thrift_websocket_server;
/** @brief Options copied or consumed during start. */
struct thrift_websocket_server_options {
    const char *listening_port; /**< One plain listener, e.g. 127.0.0.1:0; copied. */
    const char *path; /**< Exact upgrade URI beginning with /; copied. */
    size_t max_message_size; /**< Positive accumulated request/response bound, at most INT_MAX. */
    uint32_t timeout_ms; /**< Positive HTTP request and WebSocket idle/whole-frame timeout, at most INT_MAX. */
    uint32_t worker_count; /**< Concurrent connection workers, from 1 through 64. */
    enum thrift_protocol_kind kind; /**< THRIFT_BINARY or THRIFT_COMPACT. */
    const struct thrift_limits *limits; /**< Optional copied protocol limits. */
};
/** @brief Initialize CivetWeb with WebSocket support before starting workers.
 * @note Serialize all CivetWeb lifecycle calls. An application already using
 * thrift_http_server_library_init may share that lifecycle if WebSockets are enabled.
 * @return THRIFT_OK, THRIFT_INVALID if WebSockets were not compiled in, or THRIFT_IO. */
enum thrift_status thrift_websocket_server_library_init(void);
/** @brief Release CivetWeb after all servers stop.
 * @note Requires exclusive global lifecycle access.
 * @return THRIFT_OK or THRIFT_IO. */
enum thrift_status thrift_websocket_server_library_cleanup(void);
/** @brief Start a server processing one RPC per binary WebSocket message.
 * @details Continuations are assembled within max_message_size. Ping/Pong/Close
 * bypass Thrift. Responses are committed after successful complete consumption;
 * oneway calls send no response. Text/invalid messages close the connection.
 * The required CivetWeb bounded-reader extension rejects unmasked or oversized
 * frames before payload allocation. The monotonic frame deadline includes partial
 * headers and payloads; partial progress does not reset it. Complete frames reset
 * the deadline; fragmented-message duration and handler execution are not bounded.
 * This API provides plain ws://; TLS/authentication may be supplied by a reverse proxy.
 * @note Handlers run concurrently when worker_count exceeds one. The application
 * owns handler synchronization. Stop must not run inside a handler callback.
 * @param[in] options Required options.
 * @param[in] process Required synchronous processor; may implement multiplexing.
 * The protocol is borrowed only for the duration of the call.
 * @param[in,out] handler Borrowed context, alive until stop returns.
 * @param[out] result Owned server on success, NULL on failure.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, THRIFT_LIMIT or THRIFT_IO. */
enum thrift_status thrift_websocket_server_start(
    const struct thrift_websocket_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, struct thrift_websocket_server **result);
/** @brief Query the bound port, including an automatically selected port.
 * @note Safe concurrently with handlers; must not race stop.
 * @param[in] server Live server.
 * @param[out] port Required output; assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID or THRIFT_IO. */
enum thrift_status thrift_websocket_server_port(const struct thrift_websocket_server *server, uint16_t *port);
/** @brief Stop the listener, join workers and release owned state.
 * @note Requires exclusive lifecycle access; do not call from a handler. Active
 * connections are terminated; no graceful Close handshake is promised on stop.
 * @param[in,out] server Owned server, or NULL for a no-op. */
void thrift_websocket_server_stop(struct thrift_websocket_server *server);
#endif
