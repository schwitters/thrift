/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_log_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Written only during externally serialized configuration; immutable while any
 * runtime thread is active. Per-operation and delivery state is thread-local. */
static enum thrift_log_level minimum_level = THRIFT_LOG_INFO;
static thrift_log_sink configured_sink;
static void *sink_context;
static const char *const level_names[] = {"trace", "debug", "info", "warning", "error", "fatal"};
static const char *const phase_names[] = {"begin", "end", "recovery"};
static const char log_level_environment[] = "THRIFT_LOG_LEVEL";

const char *thrift_log_level_name(enum thrift_log_level level)
{
    return level >= THRIFT_LOG_TRACE && level <= THRIFT_LOG_FATAL ? level_names[level] : "invalid";
}

enum thrift_status thrift_log_parse_level(const char *name, enum thrift_log_level *level)
{
    unsigned index;
    if (!name || !level)
        return THRIFT_INVALID;
    for (index = THRIFT_LOG_TRACE; index <= THRIFT_LOG_FATAL; ++index) {
        if (!strcmp(name, level_names[index])) {
            *level = (enum thrift_log_level)index;
            return THRIFT_OK;
        }
    }
    return THRIFT_INVALID;
}

enum thrift_status thrift_log_configure(enum thrift_log_level level,
                                               thrift_log_sink sink, void *context)
{
    if (level < THRIFT_LOG_TRACE || level > THRIFT_LOG_FATAL || (!sink && context) ||
        thrift_log_thread_state()->depth || thrift_log_thread_state()->delivering)
        return THRIFT_INVALID;
    minimum_level = level;
    configured_sink = sink;
    sink_context = context;
    return THRIFT_OK;
}

enum thrift_status thrift_log_configure_level(const char *override_level)
{
    enum thrift_log_level level = THRIFT_LOG_INFO;
    const char *name = override_level ? override_level : getenv(log_level_environment);
    if (name && thrift_log_parse_level(name, &level) != THRIFT_OK)
        return THRIFT_INVALID;
    return thrift_log_configure(level, configured_sink, sink_context);
}

bool thrift_log_enabled(enum thrift_log_level level)
{
    return level >= minimum_level && level <= THRIFT_LOG_FATAL;
}

static enum thrift_status stderr_sink(void *context, const struct thrift_log_event *event)
{
    int written;
    (void)context;
    written = fprintf(stderr, "level=%s operation=%s phase=%s status=\"%s\" size=%zu native_domain=%s native_code=%" PRId64 " detail=\"%s\"\n",
        thrift_log_level_name(event->level), event->operation, phase_names[event->phase],
        thrift_status_string(event->status), event->size,
        event->native_domain ? event->native_domain : "none", event->native_code,
        event->detail ? event->detail : "");
    return written < 0 ? THRIFT_IO : THRIFT_OK;
}

void thrift_log_write(const struct thrift_log_event *event)
{
    struct thrift_log_thread_state *state = thrift_log_thread_state();
    thrift_log_sink sink = configured_sink ? configured_sink : stderr_sink;
    if (!event || !event->operation || event->level < THRIFT_LOG_TRACE ||
        event->level > THRIFT_LOG_FATAL || event->phase < THRIFT_LOG_BEGIN ||
        event->phase > THRIFT_LOG_RECOVERY || event->status < THRIFT_OK ||
        event->status >= THRIFT_STATUS_COUNT) {
        state->delivery_status = THRIFT_INVALID;
        return;
    }
    if (!thrift_log_enabled(event->level))
        return;
    if (state->delivering) {
        state->delivery_status = THRIFT_INVALID;
        return;
    }
    state->delivering = true;
    state->delivery_status = sink(sink_context, event);
    state->delivering = false;
}

struct thrift_log_scope thrift_log_begin(enum thrift_log_level level, const char *operation, size_t size)
{
    struct thrift_log_thread_state *state = thrift_log_thread_state();
    struct thrift_log_scope scope = {state->depth && level > THRIFT_LOG_DEBUG ? THRIFT_LOG_DEBUG : level, operation, size, state->depth == 0};
    struct thrift_log_event event = {scope.level, THRIFT_LOG_BEGIN, operation,
        THRIFT_OK, size, NULL, 0, NULL};
    if (scope.root) {
        state->native_domain = NULL;
        state->native_code = 0;
    }
    ++state->depth;
    thrift_log_write(&event);
    return scope;
}

void thrift_log_end(struct thrift_log_scope scope, enum thrift_status status)
{
    struct thrift_log_thread_state *state = thrift_log_thread_state();
    /* Only the outer operation owns final failure reporting. Nested operations
     * retain debug begin/end records without repeating an error during unwind. */
    struct thrift_log_event event = {
        status != THRIFT_OK && status != THRIFT_AGAIN && status != THRIFT_EOF && scope.root ? THRIFT_LOG_ERROR : scope.level,
        THRIFT_LOG_END, scope.operation, status, scope.size,
        status == THRIFT_OK ? NULL : state->native_domain,
        status == THRIFT_OK ? 0 : state->native_code, NULL};
    thrift_log_write(&event);
    --state->depth;
    if (scope.root) {
        state->native_domain = NULL;
        state->native_code = 0;
    }
}

void thrift_log_native(const char *domain, int64_t code)
{
    struct thrift_log_thread_state *state = thrift_log_thread_state();
    if (state->depth && !state->native_domain) {
        state->native_domain = domain;
        state->native_code = code;
    }
}

void thrift_log_recovered(const char *operation, const char *detail)
{
    struct thrift_log_thread_state *state = thrift_log_thread_state();
    struct thrift_log_event event = {THRIFT_LOG_WARNING, THRIFT_LOG_RECOVERY,
        operation, THRIFT_OK, 0, state->native_domain, state->native_code, detail};
    thrift_log_write(&event);
    state->native_domain = NULL;
    state->native_code = 0;
}

enum thrift_status thrift_log_last_status(void)
{
    return thrift_log_thread_state()->delivery_status;
}
