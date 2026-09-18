/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <thrift/c11/transport/thrift_socket.h>
#include <thrift/c11/transport/thrift_unix_socket.h>
#include "../thrift_log_internal.h"
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct thrift_socket { int handle; bool listener; };

static enum thrift_status socket_error(int native_error)
{
    thrift_log_native("errno", native_error);
    if (native_error == ETIMEDOUT || native_error == EAGAIN || native_error == EWOULDBLOCK)
        return THRIFT_TIMEOUT;
    if (native_error == ENOMEM || native_error == ENOBUFS)
        return THRIFT_NOMEM;
    if (native_error == EINVAL)
        return THRIFT_INVALID;
    return THRIFT_IO;
}
enum { SOCKET_BACKLOG = 16, PORT_TEXT_SIZE = 6, MILLISECONDS_PER_SECOND = 1000,
       MICROSECONDS_PER_MILLISECOND = 1000 };

static enum thrift_status unix_socket_open(const char *path, bool listener,
                                           struct thrift_socket **out)
{
    struct sockaddr_un address;
    struct thrift_socket *socket_value;
    size_t path_size, address_bytes;
    socklen_t address_size;
    int fd, result, saved_error;
    enum thrift_status status;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!path || !path[0])
        return THRIFT_INVALID;
    path_size = strnlen(path, sizeof(address.sun_path));
    if (path_size == sizeof(address.sun_path))
        return THRIFT_LIMIT;
    address_bytes = offsetof(struct sockaddr_un, sun_path) + path_size + 1;
    address_size = (socklen_t)address_bytes;
    if ((size_t)address_size != address_bytes)
        return THRIFT_LIMIT;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_size + 1);
    socket_value = calloc(1, sizeof(*socket_value));
    if (!socket_value)
        return THRIFT_NOMEM;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        saved_error = errno;
        free(socket_value);
        return socket_error(saved_error);
    }
    if (listener) {
        result = bind(fd, (const struct sockaddr *)&address, address_size);
        if (result == 0)
            result = listen(fd, SOCKET_BACKLOG);
    } else {
        result = connect(fd, (const struct sockaddr *)&address, address_size);
    }
    if (result != 0) {
        saved_error = errno;
        status = socket_error(saved_error);
        if (close(fd) != 0) {
            saved_error = errno;
            /* Preserve the opening error; close is attempted exactly once. */
            thrift_log_native("errno", saved_error);
        }
        free(socket_value);
        return status;
    }
    socket_value->handle = fd;
    socket_value->listener = listener;
    *out = socket_value;
    return THRIFT_OK;
}

enum thrift_status thrift_socket_connect_unix(const char *path, struct thrift_socket **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "socket.unix.connect", 0);
    enum thrift_status status = unix_socket_open(path, false, out);
    thrift_log_end(scope, status);
    return status;
}

enum thrift_status thrift_socket_listen_unix(const char *path, struct thrift_socket **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "socket.unix.listen", 0);
    enum thrift_status status = unix_socket_open(path, true, out);
    thrift_log_end(scope, status);
    return status;
}

static enum thrift_status socket_open(const char *host, uint16_t port, bool listener,
                                         struct thrift_socket **out)
{
    struct addrinfo hints, *addresses = NULL, *address;
    struct thrift_socket *socket_value;
    char service[PORT_TEXT_SIZE];
    size_t failed_attempts = 0;
    enum thrift_status last_error = THRIFT_IO;
    int result, fd = -1, saved_error;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!listener && (!host || !port))
        return THRIFT_INVALID;
    socket_value = calloc(1, sizeof(*socket_value));
    if (!socket_value)
        return THRIFT_NOMEM;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (result < 0 || (size_t)result >= sizeof(service)) {
        free(socket_value);
        return THRIFT_INVALID;
    }
    result = getaddrinfo(host ? host : "127.0.0.1", service, &hints, &addresses);
    saved_error = errno;
    if (result != 0) {
        free(socket_value);
        if (result == EAI_SYSTEM)
            return socket_error(saved_error);
        thrift_log_native("getaddrinfo", result);
        return result == EAI_MEMORY ? THRIFT_NOMEM : THRIFT_IO;
    }
    for (address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            saved_error = errno;
            last_error = socket_error(saved_error);
            ++failed_attempts;
            continue;
        }
        if (listener) {
            int reuse = 1;
            result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (result == 0)
                result = bind(fd, address->ai_addr, address->ai_addrlen);
            if (result == 0)
                result = listen(fd, SOCKET_BACKLOG);
        } else {
            result = connect(fd, address->ai_addr, address->ai_addrlen);
        }
        if (result == 0)
            break;
        saved_error = errno;
        last_error = socket_error(saved_error);
        ++failed_attempts;
        if (close(fd) != 0) {
            saved_error = errno;
            freeaddrinfo(addresses);
            free(socket_value);
            return socket_error(saved_error);
        }
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        free(socket_value);
        return last_error;
    }
    if (failed_attempts)
        thrift_log_recovered("socket.open", "Earlier address attempts failed; opened another resolved address.");
    socket_value->handle = fd;
    socket_value->listener = listener;
    *out = socket_value;
    return THRIFT_OK;
}

static enum thrift_status thrift_socket_connect_impl(const char *host, uint16_t port, struct thrift_socket **out)
{
    return socket_open(host, port, false, out);
}

enum thrift_status thrift_socket_connect(const char *host, uint16_t port, struct thrift_socket **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "socket.connect", 0);
    enum thrift_status status = thrift_socket_connect_impl(host, port, out);
    thrift_log_end(scope, status);
    return status;
}
static enum thrift_status thrift_socket_listen_impl(const char *host, uint16_t port, struct thrift_socket **out)
{
    return socket_open(host, port, true, out);
}

enum thrift_status thrift_socket_listen(const char *host, uint16_t port, struct thrift_socket **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "socket.listen", 0);
    enum thrift_status status = thrift_socket_listen_impl(host, port, out);
    thrift_log_end(scope, status);
    return status;
}
static enum thrift_status thrift_socket_accept_impl(struct thrift_socket *listener, struct thrift_socket **out)
{
    struct thrift_socket *socket_value;
    int fd, saved_error;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!listener || !listener->listener)
        return THRIFT_INVALID;
    socket_value = calloc(1, sizeof(*socket_value));
    if (!socket_value)
        return THRIFT_NOMEM;
    for (;;) {
        fd = accept(listener->handle, NULL, NULL);
        if (fd >= 0)
            break;
        saved_error = errno;
        if (saved_error != EINTR) {
            free(socket_value);
            return socket_error(saved_error);
        }
    }
    socket_value->handle = fd;
    *out = socket_value;
    return THRIFT_OK;
}

enum thrift_status thrift_socket_accept(struct thrift_socket *listener, struct thrift_socket **out)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "socket.accept", 0);
    enum thrift_status status = thrift_socket_accept_impl(listener, out);
    thrift_log_end(scope, status);
    return status;
}
enum thrift_status thrift_socket_port(const struct thrift_socket *socket_value, uint16_t *port)
{
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    int saved_error;
    if (!socket_value || !port)
        return THRIFT_INVALID;
    if (getsockname(socket_value->handle, (struct sockaddr *)&address, &size) != 0) {
        saved_error = errno;
        return socket_error(saved_error);
    }
    if (address.ss_family == AF_INET)
        *port = ntohs(((struct sockaddr_in *)&address)->sin_port);
    else if (address.ss_family == AF_INET6)
        *port = ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    else if (address.ss_family == AF_UNIX)
        return THRIFT_INVALID;
    else
        return THRIFT_IO;
    return THRIFT_OK;
}
static enum thrift_status socket_read_impl(void *context, void *data, size_t size)
{
    struct thrift_socket *socket_value = context;
    uint8_t *cursor = data;
    if (!socket_value || (!data && size))
        return THRIFT_INVALID;
    while (size) {
        size_t chunk = size > INT_MAX ? INT_MAX : size;
        ssize_t count = recv(socket_value->handle, cursor, chunk, 0);
        if (count < 0) {
            int saved_error = errno;
            if (saved_error == EINTR)
                continue;
            return socket_error(saved_error);
        }
        if (count == 0)
            return THRIFT_EOF;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return THRIFT_OK;
}

static enum thrift_status socket_read(void *context, void *data, size_t size)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "socket.read", size);
    enum thrift_status status = socket_read_impl(context, data, size);
    thrift_log_end(scope, status);
    return status;
}
static enum thrift_status socket_write_impl(void *context, const void *data, size_t size)
{
    struct thrift_socket *socket_value = context;
    const uint8_t *cursor = data;
    if (!socket_value || (!data && size))
        return THRIFT_INVALID;
    while (size) {
        size_t chunk = size > INT_MAX ? INT_MAX : size;
        ssize_t count = send(socket_value->handle, cursor, chunk, MSG_NOSIGNAL);
        if (count < 0) {
            int saved_error = errno;
            if (saved_error == EINTR)
                continue;
            return socket_error(saved_error);
        }
        if (count == 0)
            return THRIFT_IO;
        cursor += (size_t)count;
        size -= (size_t)count;
    }
    return THRIFT_OK;
}

static enum thrift_status socket_write(void *context, const void *data, size_t size)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_TRACE, "socket.write", size);
    enum thrift_status status = socket_write_impl(context, data, size);
    thrift_log_end(scope, status);
    return status;
}
enum thrift_status thrift_socket_transport(struct thrift_socket *socket_value,
                                                  struct thrift_transport *transport)
{
    if (!socket_value || socket_value->listener || !transport)
        return THRIFT_INVALID;
    transport->context = socket_value;
    transport->read = socket_read;
    transport->write = socket_write;
    transport->flush = NULL;
    return THRIFT_OK;
}
enum thrift_status thrift_socket_timeout(struct thrift_socket *socket_value, uint32_t milliseconds)
{
    struct timeval timeout;
    int saved_error;
    if (!socket_value || !milliseconds)
        return THRIFT_INVALID;
    timeout.tv_sec = milliseconds / MILLISECONDS_PER_SECOND;
    timeout.tv_usec = (milliseconds % MILLISECONDS_PER_SECOND) * MICROSECONDS_PER_MILLISECOND;
    if (setsockopt(socket_value->handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        saved_error = errno;
        return socket_error(saved_error);
    }
    if (setsockopt(socket_value->handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        saved_error = errno;
        return socket_error(saved_error);
    }
    return THRIFT_OK;
}
static enum thrift_status thrift_socket_close_impl(struct thrift_socket *socket_value)
{
    enum thrift_status status = THRIFT_OK;
    if (!socket_value)
        return status;
    if (close(socket_value->handle) != 0) {
        int saved_error = errno;
        status = socket_error(saved_error);
    }
    free(socket_value);
    return status;
}

enum thrift_status thrift_socket_close(struct thrift_socket *socket_value)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "socket.close", 0);
    enum thrift_status status = thrift_socket_close_impl(socket_value);
    thrift_log_end(scope, status);
    return status;
}
