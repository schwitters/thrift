/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Nonblocking TCP/TLS sockets (OpenSSL on Linux, Schannel on Windows).
 * Handles require exclusive access; independent handles may be used concurrently.
 * Configuration loads credentials synchronously. Network operations never wait;
 * THRIFT_AGAIN requests another attempt after thrift_tls_poll(). */
#ifndef THRIFT_TLS_SOCKET_H
#define THRIFT_TLS_SOCKET_H
#include <thrift/c11/transport/thrift_transport.h>
struct thrift_tls_context;
struct thrift_tls_socket;
/** @brief Credentials copied/loaded during context creation. TLS 1.2 or newer. */
struct thrift_tls_options {
    bool server; /**< Server credentials rather than a verifying client. */
    const char *certificate_file; /**< Server PEM chain on Linux, PKCS#12 file on Windows. */
    const char *private_key_file; /**< Linux PEM private key; NULL on Windows. */
    const char *password; /**< Optional key/PFX password; never retained or logged. */
    const char *ca_file; /**< Client-only trusted PEM CA on Linux, DER CA on Windows; NULL uses system roots. */
    bool machine_key; /**< Windows server: use machine CNG key storage for service accounts; false uses user storage. Linux requires false. */
};
/** @brief Load immutable credentials; out is cleared on failure.
 * Context must outlive its sockets. Creation/destruction require exclusive access;
 * one initialized context may be shared by independently owned sockets.
 * @param options Validated credential or server configuration; copied during creation.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_context_create(const struct thrift_tls_options *options,
    struct thrift_tls_context **out);
/** @brief Free credentials after all derived sockets have been destroyed. NULL is valid.
 * @param context Borrowed initialized credential context; must outlive derived sockets.
 */
void thrift_tls_context_destroy(struct thrift_tls_context *context);
/** @brief Begin TCP connect to a numeric IPv4/IPv6 address (no blocking DNS).
 * server_name is the required certificate identity (DNS name or IP) and is copied.
 * Returns OK with an owned socket; advance TCP/TLS with handshake().
 * @param context Borrowed initialized credential context; must outlive derived sockets.
 * @param address Numeric IPv4 or IPv6 endpoint; listener NULL selects loopback.
 * @param port Port number, or required output for a port query.
 * @param server_name Required DNS name or IP to verify against the server certificate.
 * @param timeout_ms Milliseconds, bounded by INT_MAX; connection deadlines must be positive.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_connect(struct thrift_tls_context *context,
    const char *address, uint16_t port, const char *server_name, uint32_t timeout_ms,
    struct thrift_tls_socket **out);
/** @brief Bind a numeric address (NULL means IPv4 loopback), allowing port zero.
 * Returns an owned nonblocking listener.
 * @param context Borrowed initialized credential context; must outlive derived sockets.
 * @param address Numeric IPv4 or IPv6 endpoint; listener NULL selects loopback.
 * @param port Port number, or required output for a port query.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_listen(struct thrift_tls_context *context,
    const char *address, uint16_t port, struct thrift_tls_socket **out);
/** @brief Accept without waiting; AGAIN means no pending connection.
 * Each accepted connection has its own positive TCP/TLS handshake deadline.
 * @param listener Live server listener requiring exclusive access.
 * @param timeout_ms Milliseconds, bounded by INT_MAX; connection deadlines must be positive.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_accept(struct thrift_tls_socket *listener,
    uint32_t timeout_ms, struct thrift_tls_socket **out);
/** @brief Query the local port; output changes only on success.
 * @param socket Live socket requiring exclusive access.
 * @param port Port number, or required output for a port query.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_port(const struct thrift_tls_socket *socket, uint16_t *port);
/** @brief Advance TCP/TLS handshake. AGAIN preserves state; failures poison the connection.
 * @param socket Live socket requiring exclusive access.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_handshake(struct thrift_tls_socket *socket);
/** @brief Receive up to capacity plaintext bytes; count is always initialized.
 * Requires completed handshake. OK transfers at least one byte for nonzero capacity.
 * AGAIN preserves state; EOF means authenticated TLS close, raw TCP truncation is IO.
 * @param socket Live socket requiring exclusive access.
 * @param data Plaintext buffer, valid for the requested byte count.
 * @param capacity Writable byte capacity of data.
 * @param count Required output receiving the transferred byte count; initialized to zero.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_read(struct thrift_tls_socket *socket, void *data,
    size_t capacity, size_t *count);
/** @brief Accept up to size plaintext bytes into bounded TLS output; count is initialized.
 * Flush before considering accepted bytes delivered. Retry only the unconsumed suffix.
 * @param socket Live socket requiring exclusive access.
 * @param data Plaintext buffer, valid for the requested byte count.
 * @param size Number of plaintext bytes available in data.
 * @param count Required output receiving the transferred byte count; initialized to zero.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_write(struct thrift_tls_socket *socket, const void *data,
    size_t size, size_t *count);
/** @brief Drain queued ciphertext without waiting. AGAIN means output remains pending.
 * @param socket Live socket requiring exclusive access.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_flush(struct thrift_tls_socket *socket);
/** @brief Send TLS close_notify incrementally; call again on AGAIN, then destroy.
 * Does not wait for peer close_notify. Do not write application data after calling.
 * @param socket Live socket requiring exclusive access.
 * @return OK on completion, AGAIN for pending nonblocking I/O, or an explicit error.
 */
enum thrift_status thrift_tls_shutdown(struct thrift_tls_socket *socket);
/** @brief Release connection immediately, without network waits. NULL is accepted.
 * @param socket Live socket requiring exclusive access.
 */
void thrift_tls_socket_destroy(struct thrift_tls_socket *socket);
/** @brief Readiness interests; returned by thrift_tls_interest after an operation. */
enum thrift_tls_event {
    THRIFT_TLS_READ = 1, /**< Wait for incoming network bytes. */
    THRIFT_TLS_WRITE = 2, /**< Wait for network output capacity. */
};
/** @brief Current retry interests; zero for NULL.
 * @param socket Live socket requiring exclusive access.
 * @return Read/write interest mask, or zero for NULL.
 */
unsigned thrift_tls_interest(const struct thrift_tls_socket *socket);
/** @brief Borrowed socket in a poll batch. No socket may be destroyed during polling. */
struct thrift_tls_poll_entry {
    struct thrift_tls_socket *socket; /**< Live borrowed socket. */
    unsigned events; /**< Requested THRIFT_TLS_READ/WRITE flags. */
    unsigned ready; /**< Returned flags; socket errors also signal requested events. */
};
/** @brief Explicit bounded wait; timeout zero only checks readiness.
 * This is the only socket API that waits. timeout_ms must fit INT_MAX.
 * @param entries Mutable poll array; NULL is valid only with zero count.
 * @param count Number of entries in the poll array.
 * @param timeout_ms Milliseconds, bounded by INT_MAX; connection deadlines must be positive.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_poll(struct thrift_tls_poll_entry *entries,
    size_t count, uint32_t timeout_ms);
/** @brief Optional synchronous framed view for existing generated clients.
 * This adapter explicitly waits using poll; the underlying TLS API remains nonblocking.
 * One flush sends one 4-byte-length-prefixed request; reads stay within one reply.
 * max_frame_size and per-send/receive deadline must be positive. One outstanding RPC.
 * The adapter borrows the socket; destroy it before the socket. */
struct thrift_tls_client;
/** @brief Create the synchronous framed adapter described above.
 * @param socket Live socket requiring exclusive access.
 * @param max_frame_size Maximum request or reply bytes, from 1 through INT32_MAX.
 * @param timeout_ms Milliseconds, bounded by INT_MAX; connection deadlines must be positive.
 * @param out Required output receiving a newly owned handle; cleared on failure.
 * @param transport Required output receiving callbacks borrowing the returned adapter.
 * @return THRIFT_OK on success, or an explicit argument, resource or platform error.
 */
enum thrift_status thrift_tls_client_create(struct thrift_tls_socket *socket,
    size_t max_frame_size, uint32_t timeout_ms, struct thrift_tls_client **out,
    struct thrift_transport *transport);
/** @brief Release the adapter buffers without closing its borrowed socket.
 * @param client Owned client adapter to release, or NULL.
 */
void thrift_tls_client_destroy(struct thrift_tls_client *client);
#endif
