/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct thrift_net { int fd; bool connecting; };
enum { PORT_CAPACITY = 6, LISTEN_BACKLOG = 128, MILLISECONDS_PER_SECOND = 1000,
       NANOSECONDS_PER_MILLISECOND = 1000000 };
static enum thrift_status net_error(int error)
{
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return THRIFT_AGAIN;
    thrift_log_native("errno", error);
    return error == ENOMEM || error == ENOBUFS ? THRIFT_NOMEM : THRIFT_IO;
}
static enum thrift_status configure(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return net_error(errno);
    return THRIFT_OK;
}
void thrift_net_destroy(struct thrift_net *net)
{
    if (!net) return;
    if (close(net->fd) != 0) { int saved = errno; thrift_log_native("errno", saved); }
    free(net);
}
enum thrift_status thrift_net_open(const char *address, uint16_t port, bool listener, struct thrift_net **out)
{
    struct addrinfo hints = {0}, *info = NULL;
    struct thrift_net *net;
    char service[PORT_CAPACITY];
    int result, saved;
    enum thrift_status status;
    *out = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    result = snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (result <= 0 || (size_t)result >= sizeof(service)) return THRIFT_INVALID;
    result = getaddrinfo(address ? address : "127.0.0.1", service, &hints, &info);
    if (result != 0) return result == EAI_MEMORY ? THRIFT_NOMEM : THRIFT_INVALID;
    net = calloc(1, sizeof(*net));
    if (!net) { freeaddrinfo(info); return THRIFT_NOMEM; }
    net->fd = socket(info->ai_family, SOCK_STREAM, 0);
    saved = errno;
    if (net->fd < 0) { free(net); freeaddrinfo(info); return net_error(saved); }
    status = configure(net->fd);
    if (status == THRIFT_OK) {
        if (listener) {
            int reuse = 1;
            result = setsockopt(net->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (!result) result = bind(net->fd, info->ai_addr, info->ai_addrlen);
            if (!result) result = listen(net->fd, LISTEN_BACKLOG);
        } else {
            result = connect(net->fd, info->ai_addr, info->ai_addrlen);
        }
        saved = errno;
        if (result < 0) {
            if (!listener && saved == EINPROGRESS) net->connecting = true;
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
    int fd = accept(listener->fd, NULL, NULL), saved = errno;
    struct thrift_net *net;
    enum thrift_status status;
    *out = NULL;
    if (fd < 0) return net_error(saved);
    net = calloc(1, sizeof(*net));
    if (!net) { if (close(fd) != 0) thrift_log_native("errno", errno); return THRIFT_NOMEM; }
    net->fd = fd;
    status = configure(fd);
    if (status != THRIFT_OK) { thrift_net_destroy(net); return status; }
    *out = net;
    return THRIFT_OK;
}
enum thrift_status thrift_net_connected(struct thrift_net *net)
{
    struct pollfd entry = {net->fd, POLLOUT, 0};
    int result, error = 0;
    socklen_t size = sizeof(error);
    if (!net->connecting) return THRIFT_OK;
    result = poll(&entry, 1, 0);
    if (result < 0) return net_error(errno);
    if (!result) return THRIFT_AGAIN;
    if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) return net_error(errno);
    if (error) return net_error(error);
    net->connecting = false;
    return THRIFT_OK;
}
enum thrift_status thrift_net_read(struct thrift_net *net, void *data, size_t size, size_t *count)
{
    ssize_t result = recv(net->fd, data, size > INT_MAX ? INT_MAX : size, 0);
    int saved = errno;
    *count = 0;
    if (result < 0) return net_error(saved);
    if (!result) return THRIFT_EOF;
    *count = (size_t)result;
    return THRIFT_OK;
}
enum thrift_status thrift_net_write(struct thrift_net *net, const void *data, size_t size, size_t *count)
{
    ssize_t result = send(net->fd, data, size > INT_MAX ? INT_MAX : size, MSG_NOSIGNAL);
    int saved = errno;
    *count = 0;
    if (result < 0) return net_error(saved);
    if (!result) return THRIFT_IO;
    *count = (size_t)result;
    return THRIFT_OK;
}
enum thrift_status thrift_net_port(const struct thrift_net *net, uint16_t *port)
{
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    if (getsockname(net->fd, (struct sockaddr *)&address, &size) != 0) return net_error(errno);
    if (address.ss_family == AF_INET) *port = ntohs(((struct sockaddr_in *)&address)->sin_port);
    else if (address.ss_family == AF_INET6) *port = ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
    else return THRIFT_IO;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_now(uint64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return net_error(errno);
    *milliseconds = (uint64_t)now.tv_sec * MILLISECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
    return THRIFT_OK;
}
enum thrift_status thrift_net_poll(struct thrift_tls_poll_entry *entries, size_t count, uint32_t timeout_ms)
{
    struct pollfd *native;
    int result, saved;
    size_t i;
    if (count > SIZE_MAX / sizeof(*native) || (nfds_t)count != count) return THRIFT_LIMIT;
    native = count ? calloc(count, sizeof(*native)) : NULL;
    if (count && !native) return THRIFT_NOMEM;
    for (i = 0; i < count; ++i) {
        native[i].fd = entries[i].socket->net->fd;
        native[i].events = (short)(((entries[i].events & THRIFT_TLS_READ) ? POLLIN : 0) |
            ((entries[i].events & THRIFT_TLS_WRITE) ? POLLOUT : 0));
    }
    result = poll(native, (nfds_t)count, (int)timeout_ms);
    saved = errno;
    if (result >= 0) for (i = 0; i < count; ++i) {
        if (native[i].revents & (POLLERR | POLLHUP | POLLNVAL)) entries[i].ready = entries[i].events;
        else entries[i].ready = ((native[i].revents & POLLIN) ? THRIFT_TLS_READ : 0) |
            ((native[i].revents & POLLOUT) ? THRIFT_TLS_WRITE : 0);
    }
    free(native);
    return result < 0 ? (saved == EINTR ? THRIFT_OK : net_error(saved)) : THRIFT_OK;
}
