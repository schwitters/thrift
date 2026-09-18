/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Nonblocking framed TLS reactor with a bounded processor worker pool. */
#ifndef THRIFT_TLS_SERVER_H
#define THRIFT_TLS_SERVER_H
#include <thrift/c11/transport/thrift_tls_socket.h>
#include <thrift/c11/protocol/thrift_protocol.h>
struct thrift_tls_server;
/** @brief Server limits. Network waits never occupy processor workers. */
struct thrift_tls_server_options {
    struct thrift_tls_context *context; /**< Borrowed server credentials, alive until destroy. */
    const char *address; /**< Numeric bind address; NULL is IPv4 loopback. */
    uint16_t port; /**< Local port, zero permits dynamic allocation. */
    size_t max_connections; /**< Positive concurrent connection ceiling, at most 4096. */
    size_t max_frame_size; /**< Positive request/response bound, at most INT32_MAX. */
    uint32_t timeout_ms; /**< Positive handshake and whole RPC deadline, at most INT_MAX. */
    size_t worker_count; /**< Fixed processor pool size, 1 through 64. */
    enum thrift_protocol_kind kind; /**< Binary or Compact; multiplexing belongs to processor. */
    const struct thrift_limits *limits; /**< Optional copied protocol resource ceilings. */
};
/** @brief Create listener and workers, without starting a reactor thread.
 * Handler/context remain borrowed until destroy. Handlers may run concurrently;
 * synchronize shared state. Processors receive memory transports, never sockets.
 * Each connection permits one active RPC. Excess connections remain in the OS
 * backlog; waiting complete requests are bounded by max_connections.
 * Out is cleared on failure; call poll repeatedly after success.
 * @param options Validated credential or server configuration; copied during creation.
 * @param process Processor callback invoked on worker threads; must return.
 * @param handler Borrowed callback state; synchronize access by concurrent workers.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_server_create(const struct thrift_tls_server_options *options,
    enum thrift_status (*process)(struct thrift_protocol *, void *), void *handler,
    struct thrift_tls_server **out);
/** @brief Service connections and completed jobs, then wait at most timeout_ms.
 * One caller owns the reactor; do not call concurrently or from a handler.
 * Peer errors close that connection and do not stop the server. Deadline expiry
 * closes the socket even while a handler runs; handlers are not forcibly cancelled.
 * A timed-out job retains its bounded slot until completion.
 * @param server Server handle requiring exclusive reactor ownership; NULL accepted by destroy.
 * @param timeout_ms Milliseconds, bounded by INT_MAX; connection deadlines must be positive.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_server_poll(struct thrift_tls_server *server, uint32_t timeout_ms);
/** @brief Query the listening port. Do not race destroy.
 * @param server Server handle requiring exclusive reactor ownership; NULL accepted by destroy.
 * @param port Port number, or required output for a port query.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_server_port(const struct thrift_tls_server *server, uint16_t *port);
/** @brief Close sockets, join all workers and release state. NULL is valid.
 * Requires exclusive reactor ownership; never call from a handler. Waits for
 * running processors, which must return; does not perform TLS shutdown I/O.
 * @param server Server handle requiring exclusive reactor ownership; NULL accepted by destroy.
 */
void thrift_tls_server_destroy(struct thrift_tls_server *server);
#endif
