/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Http server public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_HTTP_SERVER_H
#define THRIFT_HTTP_SERVER_H
#include <thrift/c11/processor/thrift_processor.h>

/** @brief Opaque owned CivetWeb server; stop joins its worker before freeing it. */
struct thrift_http_server;

/**
 * @brief Initialize the process-wide CivetWeb library.
 *
 * @note Call before starting worker threads. Serialize all global lifecycle calls,
 * including direct CivetWeb calls. Do not mix uncoordinated direct CivetWeb lifecycle calls.
 * @return THRIFT_OK on success; THRIFT_INVALID if no non-TLS CivetWeb feature is available, or THRIFT_IO if initialization fails.
 */
enum thrift_status thrift_http_server_library_init(void);
/**
 * @brief Release process-wide CivetWeb library state.
 *
 * @pre Stop all servers and worker threads first.
 * @note Requires serialized process-wide lifecycle access; not concurrent-safe.
 * @return THRIFT_OK on success, or THRIFT_IO on cleanup failure.
 */
enum thrift_status thrift_http_server_library_cleanup(void);

/** @brief HTTP server configuration consumed or copied during startup.
 * Requires the THRIFT_C11_HTTP_SERVER build option. */
struct thrift_http_server_options {
    /** @brief Single CivetWeb address:port (for example 127.0.0.1:0); no SSL/redirect suffixes or
     * listener lists.
     */
    const char *listening_port;
    const char *path; /**< Exact URI path beginning with a slash, copied during start. */
    size_t max_body_size; /**< Positive independent request/response byte bound, at most LLONG_MAX. */
    uint32_t timeout_ms; /**< Positive CivetWeb request timeout in milliseconds, at most INT_MAX. */
    enum thrift_protocol_kind kind; /**< THRIFT_BINARY or THRIFT_COMPACT. */
    /** @brief Optional borrowed protocol limits copied during start; NULL uses defaults.
     */
    const struct thrift_limits *limits;
};

/**
 * @brief Start an HTTP-only CivetWeb server with one worker.
 *
 * @pre Initialize the server library before starting workers.
 * Only the exact configured path is served; no static files are exposed. Binary,
 * Compact and multiplexed processors can be used. HTTPS requires external TLS
 * termination. The processor runs on the worker and must not call stop.
 * @note Independent servers may run concurrently. Synchronize handler access from
 * other threads and configure logging before server startup.
 *
 * @param[in] options Required options; strings/settings are consumed or copied during start.
 * @param[in] process Non-NULL processor called once per matching POST.
 * @param[in,out] handler Borrowed callback context; may be NULL if process accepts it. Retain until stop returns.
 * @param[out] result Owned server handle on success; cleared to NULL before setup.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, THRIFT_LIMIT, or THRIFT_IO.
 */
enum thrift_status thrift_http_server_start(
    const struct thrift_http_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *),
    void *handler, struct thrift_http_server **result);

/**
 * @brief Query the listening port selected by CivetWeb.
 *
 * Useful when listening_port specifies port zero. Do not race this call with stop.
 *
 * @param[in] server Live server; exclusive access to the handle is required.
 * @param[out] port Required output, assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID, or THRIFT_IO.
 */
enum thrift_status thrift_http_server_port(const struct thrift_http_server *server,
                                                  uint16_t *port);
/**
 * @brief Stop a server, join its worker and release the handle.
 *
 * Blocks until the worker exits; the borrowed handler may be released afterwards.
 * @warning Never call from the server processor callback.
 * @note Requires exclusive access to the handle; do not race with queries or another stop.
 *
 * @param[in,out] server Owned server; NULL is a no-op.
 */
void thrift_http_server_stop(struct thrift_http_server *server);
#endif
