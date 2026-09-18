/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct thrift_net { SOCKET fd; bool connecting; };
enum { PORT_CAPACITY = 6, LISTEN_BACKLOG = 128 };
static enum thrift_status net_error(int error)
{
    if (error == WSAEWOULDBLOCK || error == WSAEINTR) return THRIFT_AGAIN;
    thrift_log_native("winsock", error);
    return error == WSAENOBUFS ? THRIFT_NOMEM : THRIFT_IO;
}
static enum thrift_status configure(SOCKET fd)
{
    u_long enabled = 1;
    if (ioctlsocket(fd, FIONBIO, &enabled) != 0) return net_error(WSAGetLastError());
    return THRIFT_OK;
}
static void cleanup_winsock(void)
{
    if (WSACleanup() != 0) { int saved = WSAGetLastError(); thrift_log_native("winsock", saved); }
}
void thrift_net_destroy(struct thrift_net *net)
{
    if (!net) return;
    if (closesocket(net->fd) != 0) { int saved = WSAGetLastError(); thrift_log_native("winsock", saved); }
    cleanup_winsock();
    free(net);
}
enum thrift_status thrift_net_open(const char *address, uint16_t port, bool listener, struct thrift_net **out)
{
    struct addrinfo hints = {0}, *info = NULL;
    struct thrift_net *net;
    char service[PORT_CAPACITY];
    int result, saved;
    WSADATA startup;
    enum thrift_status status;
    *out = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    result = snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (result <= 0 || (size_t)result >= sizeof(service)) return THRIFT_INVALID;
    result = WSAStartup(MAKEWORD(2, 2), &startup);
    if (result) return net_error(result);
    result = getaddrinfo(address ? address : "127.0.0.1", service, &hints, &info);
    if (result != 0) { cleanup_winsock(); return THRIFT_INVALID; }
    net = calloc(1, sizeof(*net));
    if (!net) { freeaddrinfo(info); cleanup_winsock(); return THRIFT_NOMEM; }
    net->fd = socket(info->ai_family, SOCK_STREAM, 0);
    saved = WSAGetLastError();
    if (net->fd == INVALID_SOCKET) { free(net); freeaddrinfo(info); cleanup_winsock(); return net_error(saved); }
    status = configure(net->fd);
    if (status == THRIFT_OK) {
        if (listener) {
            int reuse = 1;
            result = setsockopt(net->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&reuse, sizeof(reuse));
            if (!result) result = bind(net->fd, info->ai_addr, (int)info->ai_addrlen);
            if (!result) result = listen(net->fd, LISTEN_BACKLOG);
        } else {
            result = connect(net->fd, info->ai_addr, (int)info->ai_addrlen);
        }
        saved = WSAGetLastError();
        if (result < 0) {
            if (!listener && saved == WSAEWOULDBLOCK) net->connecting = true;
            else status = net_error(saved);
        }
    }
    freeaddrinfo(info);
    if (status != THRIFT_OK) { thrift_net_destroy(net); return status; }
    *out = net;
    return THRIFT_OK;
}
enum thrift_status thrift_net_accept(struct thrift_net *listener, struct thrift_net **out)
{
    SOCKET fd;
    int saved;
    WSADATA startup;
    struct thrift_net *net;
    enum thrift_status status;
    *out = NULL;
    saved = WSAStartup(MAKEWORD(2, 2), &startup);
    if (saved) return net_error(saved);
    fd = accept(listener->fd, NULL, NULL);
    saved = WSAGetLastError();
    if (fd == INVALID_SOCKET) { cleanup_winsock(); return net_error(saved); }
    net = calloc(1, sizeof(*net));
    if (!net) { if (closesocket(fd) != 0) thrift_log_native("winsock", WSAGetLastError()); cleanup_winsock(); return THRIFT_NOMEM; }
    net->fd = fd;
    status = configure(fd);
    if (status != THRIFT_OK) { thrift_net_destroy(net); return status; }
    *out = net;
    return THRIFT_OK;
}
enum thrift_status thrift_net_connected(struct thrift_net *net)
{
    WSAPOLLFD entry = {net->fd, POLLOUT, 0};
    int result, error = 0;
    int size = sizeof(error);
    if (!net->connecting) return THRIFT_OK;
    result = WSAPoll(&entry, 1, 0);
    if (result < 0) return net_error(WSAGetLastError());
    if (!result) return THRIFT_AGAIN;
    if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, (char *)&error, &size) != 0) return net_error(WSAGetLastError());
    if (error) return net_error(error);
    net->connecting = false;
    return THRIFT_OK;
}
enum thrift_status thrift_net_read(struct thrift_net *net, void *data, size_t size, size_t *count)
{
    int result = recv(net->fd, data, size > INT_MAX ? INT_MAX : (int)size, 0);
    int saved = WSAGetLastError();
    *count = 0;
    if (result < 0) return net_error(saved);
    if (!result) return THRIFT_EOF;
    *count = (size_t)result;
    return THRIFT_OK;
}
enum thrift_status thrift_net_write(struct thrift_net *net, const void *data, size_t size, size_t *count)
{
    int result = send(net->fd, data, size > INT_MAX ? INT_MAX : (int)size, 0);
    int saved = WSAGetLastError();
    *count = 0;
    if (result < 0) return net_error(saved);
    if (!result) return THRIFT_IO;
    *count = (size_t)result;
    return THRIFT_OK;
}
enum thrift_status thrift_net_port(const struct thrift_net *net, uint16_t *port)
{
    struct sockaddr_storage address;
    int size = sizeof(address);
    if (getsockname(net->fd, (struct sockaddr *)&address, &size) != 0) return net_error(WSAGetLastError());
    if (address.ss_family == AF_INET) *port = ntohs(((struct sockaddr_in *)&address)->sin_port);
    else if (address.ss_family == AF_INET6) *port = ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    else return THRIFT_IO;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_now(uint64_t *milliseconds)
{
    *milliseconds = GetTickCount64();
    return THRIFT_OK;
}
enum thrift_status thrift_net_poll(struct thrift_tls_poll_entry *entries, size_t count, uint32_t timeout_ms)
{
    WSAPOLLFD *native;
    int result, saved;
    size_t i;
    if (count > SIZE_MAX / sizeof(*native) || count > ULONG_MAX) return THRIFT_LIMIT;
    native = count ? calloc(count, sizeof(*native)) : NULL;
    if (count && !native) return THRIFT_NOMEM;
    for (i = 0; i < count; ++i) {
        native[i].fd = entries[i].socket->net->fd;
        native[i].events = (short)(((entries[i].events & THRIFT_TLS_READ) ? POLLIN : 0) |
            ((entries[i].events & THRIFT_TLS_WRITE) ? POLLOUT : 0));
    }
    if (!count) { Sleep(timeout_ms); free(native); return THRIFT_OK; }
    result = WSAPoll(native, (ULONG)count, (int)timeout_ms);
    saved = WSAGetLastError();
    if (result >= 0) for (i = 0; i < count; ++i) {
        if (native[i].revents & (POLLERR | POLLHUP | POLLNVAL)) entries[i].ready = entries[i].events;
        else entries[i].ready = ((native[i].revents & POLLIN) ? THRIFT_TLS_READ : 0) |
            ((native[i].revents & POLLOUT) ? THRIFT_TLS_WRITE : 0);
    }
    free(native);
    return result < 0 ? (saved == WSAEINTR ? THRIFT_OK : net_error(saved)) : THRIFT_OK;
}
