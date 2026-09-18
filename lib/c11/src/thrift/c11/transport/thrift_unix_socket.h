/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief POSIX-only Unix-domain stream sockets using the common socket API.
 *
 * These declarations and implementations are not installed/built for Windows.
 * Paths name filesystem sockets; Linux abstract addresses are not supported.
 * The application owns the pathname: neither listen nor close unlinks it.
 * Use a directory controlled by the application and explicitly remove its socket
 * pathname after closing the listener. A stale pathname prevents rebinding.
 */
#ifndef THRIFT_UNIX_SOCKET_H
#define THRIFT_UNIX_SOCKET_H
#include <thrift/c11/transport/thrift_socket.h>

/**
 * @brief Connect to a filesystem Unix-domain stream socket.
 * @param[in] path Nonempty NUL-terminated native pathname, copied during the call.
 * @param[out] out Required owned-handle destination; cleared to NULL before setup.
 * @return THRIFT_OK, THRIFT_INVALID for invalid arguments, THRIFT_LIMIT if the path
 * does not fit sockaddr_un.sun_path with a terminator, or an allocation/I/O error.
 *
 * Relative paths resolve against the current working directory. Connect blocks;
 * thrift_socket_timeout() configures subsequent I/O, not connection establishment.
 * Use thrift_socket_transport() and thrift_socket_close() on the returned handle.
 * @note Independent handles may be used concurrently. Synchronize shared handles
 * and changes to the working directory when using relative paths.
 */
enum thrift_status thrift_socket_connect_unix(const char *path, struct thrift_socket **out);

/**
 * @brief Bind and listen on a filesystem Unix-domain stream socket.
 * @param[in] path Nonempty NUL-terminated native pathname, copied during the call.
 * @param[out] out Required owned-listener destination; cleared to NULL before setup.
 * @return THRIFT_OK, THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for an
 * overlong pathname, or an allocation/I/O error (including an occupied path).
 *
 * Existing files, symlinks and sockets are never removed. Socket permissions
 * follow the process umask; this function does not change it. The pathname remains
 * after close, and can also remain if bind succeeds but listen subsequently fails.
 * Its lifetime and explicit removal belong to the application. Accept with
 * thrift_socket_accept() or use thrift_server_serve_kind(). Closing an accepted
 * connection does not affect the listener or pathname. Relative paths resolve
 * against the current working directory. thrift_socket_port() returns THRIFT_INVALID
 * for Unix-domain sockets because they have no TCP port.
 * @note Independent handles may be used concurrently. Synchronize shared handles,
 * pathname management and working-directory changes.
 */
enum thrift_status thrift_socket_listen_unix(const char *path, struct thrift_socket **out);
#endif
