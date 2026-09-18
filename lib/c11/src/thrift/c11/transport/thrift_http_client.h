/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Http client public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_HTTP_CLIENT_H
#define THRIFT_HTTP_CLIENT_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Opaque owned libcurl client; exclusive access is required per instance. */
struct thrift_http_client;

/**
 * @brief Initialize the process-wide libcurl library.
 *
 * @note Call before starting worker threads. Serialize all global lifecycle calls,
 * including direct libcurl calls. Applications already managing libcurl globally may use its lifecycle API.
 * @return THRIFT_OK on success; THRIFT_IO if library initialization fails.
 */
enum thrift_status thrift_http_client_library_init(void);
/**
 * @brief Release process-wide libcurl library state.
 *
 * @pre Stop all clients and worker threads first.
 * @note Requires serialized process-wide lifecycle access; not concurrent-safe.
 */
void thrift_http_client_library_cleanup(void);

/** @brief HTTP client configuration copied or consumed during creation.
 * Requires the THRIFT_C11_HTTP_CLIENT build option. */
struct thrift_http_client_options {
    const char *url; /**< Required HTTP/HTTPS URL; copied by libcurl during create. */
    size_t max_body_size; /**< Positive independent request/response byte bound, at most LONG_MAX. */
    /** @brief Positive connect/whole-transfer timeout in milliseconds, at most INT_MAX; DNS behavior
     * depends on the libcurl resolver.
     */
    uint32_t timeout_ms;
    const char *ca_file; /**< Optional CA bundle file path, copied during create; NULL uses libcurl defaults. */
};

/**
 * @brief Create a buffered libcurl HTTP/HTTPS client transport.
 *
 * @pre Initialize libcurl globally before use.
 * Writes accumulate; flush sends one POST and buffers the full response. Only HTTP
 * 200 succeeds; other statuses return THRIFT_REMOTE. Redirects are disabled and TLS
 * certificate/hostname verification is enabled. Each flush discards the previous
 * unread response. On errors discard the instance.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] options Required options; URL and optional CA path are copied by libcurl.
 * @param[out] result Owned handle on success; set to NULL when both output pointers are valid.
 * @param[out] transport Borrowed view of the handle, assigned only on success.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, or THRIFT_IO.
 */
enum thrift_status thrift_http_client_create(
    const struct thrift_http_client_options *options,
    struct thrift_http_client **result, struct thrift_transport *transport);
/** @brief Maximum Bearer token length in bytes, excluding the terminating NUL. */
enum { THRIFT_HTTP_BEARER_TOKEN_MAX = 16384 };
/**
 * @brief Set, replace or remove the Bearer token for subsequent HTTP requests.
 *
 * No request is sent. libcurl copies the token during this call; the caller retains
 * ownership of its input. Tokens are never logged. Use HTTPS for remote credentials.
 * Redirects remain disabled. This does not implement login or automatic refresh.
 * @note Requires exclusive access to the client, including its transport views.
 * @param[in,out] client Live client. Discard it after THRIFT_IO or THRIFT_NOMEM.
 * @param[in] token NUL-terminated RFC 6750 token, at most THRIFT_HTTP_BEARER_TOKEN_MAX
 * bytes, or NULL to remove authentication. Empty strings and invalid characters
 * are rejected without changing the previous token.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT, THRIFT_NOMEM, THRIFT_IO,
 * or the client's previous transport error.
 */
enum thrift_status thrift_http_client_set_bearer_token(
    struct thrift_http_client *client, const char *token);
/**
 * @brief Release a client and invalidate all its transport views.
 *
 * Does not flush buffered requests. Must finish before global libcurl cleanup.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] client Owned client; NULL is a no-op.
 */
void thrift_http_client_destroy(struct thrift_http_client *client);
#endif
