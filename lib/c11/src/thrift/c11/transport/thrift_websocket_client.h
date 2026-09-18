/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Synchronous libcurl WebSocket client transport.
 */
#ifndef THRIFT_WEBSOCKET_CLIENT_H
#define THRIFT_WEBSOCKET_CLIENT_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Owned connection; each instance requires exclusive access. */
struct thrift_websocket_client;
/** @brief Connection options, copied or consumed by create. */
struct thrift_websocket_client_options {
    const char *url; /**< Required ws:// or wss:// endpoint; copied. */
    size_t max_message_size; /**< Positive request/response message bound, at most INT_MAX. */
    uint32_t timeout_ms; /**< Positive connect and per-read/flush/close timeout, at most INT_MAX. */
    const char *ca_file; /**< Optional copied CA bundle; normal TLS verification stays enabled. */
    const char *bearer_token; /**< Optional copied RFC 6750 token for the upgrade request; never logged. */
};
/** @brief Initialize libcurl before workers start.
 * @note Serialize global lifecycle calls. Applications already managing libcurl
 * (including thrift_http_client_library_init) may use that lifecycle instead.
 * @return THRIFT_OK or THRIFT_IO. */
enum thrift_status thrift_websocket_client_library_init(void);
/** @brief Release libcurl global state after all clients and workers stop.
 * @note Requires process-wide exclusive lifecycle access. */
void thrift_websocket_client_library_cleanup(void);
/** @brief Connect and perform the WebSocket upgrade.
 * @details Each nonempty flush sends one binary WebSocket message. Reads collect
 * one complete bounded reply, including continuation frames; reads never cross a
 * message boundary. Empty messages are ignored. Flush does not await a reply,
 * allowing oneway calls. Use sequential synchronous RPCs, not pipelined calls.
 * Text messages are rejected. Ping replies are handled while receiving.
 * Redirects and extensions are disabled. Use wss:// for remote credentials.
 * @note Requires global libcurl initialization. Independent instances may run
 * concurrently; synchronize access to a shared instance and its transport views.
 * @param[in] options Required options; no pointer is retained after this call.
 * @param[out] result Owned client on success, NULL on failure when outputs are valid.
 * @param[out] transport Borrowed transport view; assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT, THRIFT_NOMEM, THRIFT_TIMEOUT,
 * THRIFT_REMOTE for a rejected upgrade, or THRIFT_IO. */
enum thrift_status thrift_websocket_client_create(
    const struct thrift_websocket_client_options *options,
    struct thrift_websocket_client **result, struct thrift_transport *transport);
/** @brief Send normal Close and wait for the peer's Close within the timeout.
 * @note Requires exclusive instance access. Destroy the client afterwards,
 * regardless of status. This does not flush buffered application writes.
 * @param[in,out] client Live client; repeated close after success is harmless.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_TIMEOUT, or a transport/protocol error. */
enum thrift_status thrift_websocket_client_close(struct thrift_websocket_client *client);
/** @brief Release the connection and all owned buffers without blocking on Close.
 * @note Requires exclusive access; invalidates all transport views.
 * @param[in,out] client Owned client, or NULL for a no-op. */
void thrift_websocket_client_destroy(struct thrift_websocket_client *client);
#endif
