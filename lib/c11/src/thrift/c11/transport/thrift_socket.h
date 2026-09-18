/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Socket public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_SOCKET_H
#define THRIFT_SOCKET_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Opaque owned blocking socket handle; release with thrift_socket_close().
 * Supports TCP and, on POSIX, Unix-domain streams via thrift_unix_socket.h.
 * Independent handles are reentrant. Never close/access one handle concurrently.
 * Each Windows handle owns its Winsock initialization reference. */
struct thrift_socket;
/**
 * @brief Open a blocking TCP connection.
 *
 * Release the handle with thrift_socket_close(). Connection establishment is not
 * bounded by thrift_socket_timeout(), which configures subsequent I/O.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] host Non-NULL DNS name or numeric address.
 * @param[in] port Nonzero remote port.
 * @param[out] out Required owned-handle destination; cleared to NULL before acquisition.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, or a socket/resolver error.
 */
enum thrift_status thrift_socket_connect(const char *host, uint16_t port,
                                                struct thrift_socket **out);
/**
 * @brief Create a blocking TCP listener.
 *
 * Use thrift_socket_port() to query the selected port. Close the owned listener
 * with thrift_socket_close().
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] host Bind address; NULL selects IPv4 loopback.
 * @param[in] port Local port; zero requests an available port.
 * @param[out] out Required owned-handle destination; cleared to NULL before acquisition.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, or a socket/resolver error.
 */
enum thrift_status thrift_socket_listen(const char *host, uint16_t port,
                                               struct thrift_socket **out);
/**
 * @brief Accept one connection from a blocking listener.
 *
 * The caller retains the listener and closes the returned connection independently.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] listener Live listening socket.
 * @param[out] out Required destination for a separately owned connected socket; cleared to NULL.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NOMEM, or a socket error.
 */
enum thrift_status thrift_socket_accept(struct thrift_socket *listener,
                                               struct thrift_socket **out);
/**
 * @brief Query the local TCP port.
 *
 * For connected sockets this is the local port, not the remote port.
 * Unix-domain sockets have no TCP port and return THRIFT_INVALID without changing port.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] socket Live listener or connected socket.
 * @param[out] port Required destination, written only on success.
 * @return THRIFT_OK, THRIFT_INVALID, or a socket error.
 */
enum thrift_status thrift_socket_port(const struct thrift_socket *socket,
                                             uint16_t *port);
/**
 * @brief Create a borrowed transport view of a connected socket.
 *
 * The view does not own or close the socket. Exact-size reads/writes can partially
 * transfer before failure; close the connection after an I/O error.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] socket Live connected socket, not a listener; must outlive transport use.
 * @param[out] transport Destination view; contains no flush callback.
 * @return THRIFT_OK, or THRIFT_INVALID.
 */
enum thrift_status thrift_socket_transport(struct thrift_socket *socket,
                                                  struct thrift_transport *transport);

/**
 * @brief Set per-blocking-read/write timeouts.
 *
 * Does not bound the whole RPC or initial connection establishment. If setting one
 * direction fails, the other timeout may already have changed.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] socket Live socket.
 * @param[in] milliseconds Positive timeout in milliseconds.
 * @return THRIFT_OK, THRIFT_INVALID, or a platform socket error.
 */
enum thrift_status thrift_socket_timeout(struct thrift_socket *socket,
                                                uint32_t milliseconds);
/**
 * @brief Close platform resources and release a socket handle.
 *
 * The handle is invalid after this call even if closing reports an error. All
 * borrowed transport views become invalid; do not retry with the freed handle.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] socket Owned handle to consume; NULL is accepted.
 * @return THRIFT_OK for success/NULL, or a platform close error.
 */
enum thrift_status thrift_socket_close(struct thrift_socket *socket);
#endif
