/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_LOG_INTERNAL_H
#define THRIFT_LOG_INTERNAL_H
#include <thrift/c11/thrift_log.h>
struct thrift_log_thread_state {
    unsigned depth;
    bool delivering;
    const char *native_domain;
    int64_t native_code;
    enum thrift_status delivery_status;
};
struct thrift_log_scope {
    enum thrift_log_level level;
    const char *operation;
    size_t size;
    bool root;
};
struct thrift_log_thread_state *thrift_log_thread_state(void);
struct thrift_log_scope thrift_log_begin(enum thrift_log_level level, const char *operation, size_t size);
void thrift_log_end(struct thrift_log_scope scope, enum thrift_status status);
/* Capture the first underlying failure before cleanup changes the native error. */
void thrift_log_native(const char *domain, int64_t code);
void thrift_log_recovered(const char *operation, const char *detail);
#endif
