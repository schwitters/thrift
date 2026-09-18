/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/transport/thrift_socket.h>
#include "../thrift_log_internal.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each live handle owns one WSAStartup reference. Winsock serializes its
 * process-wide reference count, so independent handles require no global state. */
struct thrift_socket { SOCKET handle; bool listener; };
enum { WINSOCK_MAJOR_VERSION = 2, WINSOCK_MINOR_VERSION = 2 };

static enum thrift_status socket_error(int native_error)
{
    thrift_log_native("winsock", native_error);
    if (native_error == WSAETIMEDOUT || native_error == WSAEWOULDBLOCK)
        return THRIFT_TIMEOUT;
    if (native_error == WSAENOBUFS)
        return THRIFT_NOMEM;
    if (native_error == WSAEINVAL)
        return THRIFT_INVALID;
    return THRIFT_IO;
}
static enum thrift_status winsock_start(void)
{
    WSADATA data;
    int result = WSAStartup(MAKEWORD(WINSOCK_MAJOR_VERSION, WINSOCK_MINOR_VERSION), &data);
    return result == 0 ? THRIFT_OK : socket_error(result);
}
static enum thrift_status winsock_release(enum thrift_status status)
{
    if (WSACleanup() != 0) {
        int saved_error = WSAGetLastError();
        return socket_error(saved_error);
    }
    return status;
}
enum { SOCKET_BACKLOG = 16, PORT_TEXT_SIZE = 6 };

static enum thrift_status socket_open(const char *host, uint16_t port, bool listener,
                                         struct thrift_socket **out)
{
    struct addrinfo hints, *addresses = NULL, *address;
    struct thrift_socket *socket_value;
    char service[PORT_TEXT_SIZE];
    size_t failed_attempts = 0;
    enum thrift_status last_error = THRIFT_IO;
    int result, saved_error;
    SOCKET fd = INVALID_SOCKET;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!listener && (!host || !port))
        return THRIFT_INVALID;
    if (winsock_start() != THRIFT_OK)
        return THRIFT_IO;
    socket_value = calloc(1, sizeof(*socket_value));
    if (!socket_value)
        return winsock_release(THRIFT_NOMEM);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    result = snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (result < 0 || (size_t)result >= sizeof(service)) {
        free(socket_value);
        return winsock_release(THRIFT_INVALID);
    }
    result = getaddrinfo(host ? host : "127.0.0.1", service, &hints, &addresses);
    if (result != 0) {
        free(socket_value);
        return winsock_release(socket_error(result));
    }
    for (address = addresses; address; address = address->ai_next) {
        if (address->ai_addrlen > INT_MAX)
            continue;
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == INVALID_SOCKET) {
            saved_error = WSAGetLastError();
            last_error = socket_error(saved_error);
            ++failed_attempts;
            continue;
        }
        if (listener) {
            int reuse = 1;
            result = setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&reuse, sizeof(reuse));
            if (result == 0)
                result = bind(fd, address->ai_addr, (int)address->ai_addrlen);
            if (result == 0)
                result = listen(fd, SOCKET_BACKLOG);
        } else {
            result = connect(fd, address->ai_addr, (int)address->ai_addrlen);
        }
        if (result == 0)
            break;
        saved_error = WSAGetLastError();
        last_error = socket_error(saved_error);
        ++failed_attempts;
        if (closesocket(fd) != 0) {
            saved_error = WSAGetLastError();
            freeaddrinfo(addresses);
            free(socket_value);
            return winsock_release(socket_error(saved_error));
        }
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (fd == INVALID_SOCKET) {
        free(socket_value);
        return winsock_release(last_error);
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
    SOCKET fd;
    int saved_error;
    if (!out)
        return THRIFT_INVALID;
    *out = NULL;
    if (!listener || !listener->listener)
        return THRIFT_INVALID;
    if (winsock_start() != THRIFT_OK)
        return THRIFT_IO;
    socket_value = calloc(1, sizeof(*socket_value));
    if (!socket_value)
        return winsock_release(THRIFT_NOMEM);
    for (;;) {
        fd = accept(listener->handle, NULL, NULL);
        if (fd != INVALID_SOCKET)
            break;
        saved_error = WSAGetLastError();
        if (saved_error != WSAEINTR) {
            free(socket_value);
            return winsock_release(socket_error(saved_error));
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
    int size = sizeof(address);
    int saved_error;
    if (!socket_value || !port)
        return THRIFT_INVALID;
    if (getsockname(socket_value->handle, (struct sockaddr *)&address, &size) != 0) {
        saved_error = WSAGetLastError();
        return socket_error(saved_error);
    }
    if (address.ss_family == AF_INET)
        *port = ntohs(((struct sockaddr_in *)&address)->sin_port);
    else if (address.ss_family == AF_INET6)
        *port = ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
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
        int chunk = size > INT_MAX ? INT_MAX : (int)size;
        int count = recv(socket_value->handle, (char *)cursor, chunk, 0);
        if (count < 0) {
            int saved_error = WSAGetLastError();
            if (saved_error == WSAEINTR)
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
        int chunk = size > INT_MAX ? INT_MAX : (int)size;
        int count = send(socket_value->handle, (const char *)cursor, chunk, 0);
        if (count < 0) {
            int saved_error = WSAGetLastError();
            if (saved_error == WSAEINTR)
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
    DWORD timeout;
    int saved_error;
    if (!socket_value || !milliseconds)
        return THRIFT_INVALID;
    timeout = milliseconds;
    if (setsockopt(socket_value->handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0) {
        saved_error = WSAGetLastError();
        return socket_error(saved_error);
    }
    if (setsockopt(socket_value->handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout)) != 0) {
        saved_error = WSAGetLastError();
        return socket_error(saved_error);
    }
    return THRIFT_OK;
}
static enum thrift_status thrift_socket_close_impl(struct thrift_socket *socket_value)
{
    enum thrift_status status = THRIFT_OK;
    if (!socket_value)
        return status;
    if (closesocket(socket_value->handle) != 0) {
        int saved_error = WSAGetLastError();
        status = socket_error(saved_error);
    }
    free(socket_value);
    return winsock_release(status);
}

enum thrift_status thrift_socket_close(struct thrift_socket *socket_value)
{
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_DEBUG, "socket.close", 0);
    enum thrift_status status = thrift_socket_close_impl(socket_value);
    thrift_log_end(scope, status);
    return status;
}
