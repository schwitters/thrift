/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_TLS_INTERNAL_H
#define THRIFT_TLS_INTERNAL_H
#include "thrift_tls_socket.h"
#include "thrift_buffer_internal.h"
struct thrift_net;
struct thrift_tls_engine {
    void *state;
    struct thrift_buffer output;
    bool ready;
};
enum { THRIFT_TLS_CIPHER_LIMIT = 256 * 1024, THRIFT_TLS_RECORD_SIZE = 16384,
       THRIFT_TLS_FRAME_HEADER = 4, THRIFT_TLS_WORKER_LIMIT = 64,
       THRIFT_TLS_POLL_QUANTUM_MS = 10 };
struct thrift_tls_socket {
    struct thrift_net *net;
    struct thrift_tls_context *context;
    struct thrift_tls_engine engine;
    uint64_t deadline;
    enum thrift_status error;
    unsigned interest;
    bool listener, connecting, closing, established;
};
bool thrift_tls_context_server(const struct thrift_tls_context *context);
enum thrift_status thrift_tls_engine_create(struct thrift_tls_context *context,
    const char *name, struct thrift_tls_engine *engine);
void thrift_tls_engine_destroy(struct thrift_tls_engine *engine);
enum thrift_status thrift_tls_engine_feed(struct thrift_tls_engine *engine, const void *data, size_t size);
enum thrift_status thrift_tls_engine_handshake(struct thrift_tls_engine *engine);
enum thrift_status thrift_tls_engine_read(struct thrift_tls_engine *engine, void *data, size_t size, size_t *count);
enum thrift_status thrift_tls_engine_write(struct thrift_tls_engine *engine, const void *data, size_t size, size_t *count);
enum thrift_status thrift_tls_engine_shutdown(struct thrift_tls_engine *engine);
enum thrift_status thrift_net_open(const char *address, uint16_t port, bool listen, struct thrift_net **out);
enum thrift_status thrift_net_accept(struct thrift_net *listener, struct thrift_net **out);
enum thrift_status thrift_net_connected(struct thrift_net *net);
enum thrift_status thrift_net_read(struct thrift_net *net, void *data, size_t size, size_t *count);
enum thrift_status thrift_net_write(struct thrift_net *net, const void *data, size_t size, size_t *count);
enum thrift_status thrift_net_port(const struct thrift_net *net, uint16_t *port);
void thrift_net_destroy(struct thrift_net *net);
enum thrift_status thrift_tls_now(uint64_t *milliseconds);
/* The platform implementation accesses socket->net without exposing native handles. */
enum thrift_status thrift_net_poll(struct thrift_tls_poll_entry *entries, size_t count, uint32_t timeout_ms);
struct thrift_tls_worker;
enum thrift_status thrift_tls_worker_create(void (*run)(void *), struct thrift_tls_worker **out);
enum thrift_status thrift_tls_worker_submit(struct thrift_tls_worker *worker, void *job);
bool thrift_tls_worker_take(struct thrift_tls_worker *worker, void **job);
void thrift_tls_worker_destroy(struct thrift_tls_worker *worker);
#endif
