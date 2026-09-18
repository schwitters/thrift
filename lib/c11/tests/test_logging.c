/* SPDX-License-Identifier: Apache-2.0 */
#include <thrift/c11/thrift_log.h>
#include <thrift/c11/transport/thrift_socket.h>
#include "../src/thrift/c11/thrift_log_internal.h"
#include "test_logging_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
enum { EVENT_CAPACITY = 16, INNER_FAILURE_CODE = 17, CLEANUP_FAILURE_CODE = 19,
       WORKER_FAILURE_CODE = 23, MAIN_FAILURE_CODE = 29,
       OUTER_BEGIN_INDEX = 0, INNER_BEGIN_INDEX = 1, INNER_END_INDEX = 2, OUTER_END_INDEX = 3,
       NESTED_EVENT_COUNT = 4 };
struct capture {
    struct thrift_log_event events[EVENT_CAPACITY];
    size_t count;
    enum thrift_status delivery;
    bool worker_ok;
};
static enum thrift_status capture_event(void *context, const struct thrift_log_event *event)
{
    struct capture *capture = context;
    if (capture->count >= EVENT_CAPACITY)
        return THRIFT_LIMIT;
    capture->events[capture->count++] = *event;
    return capture->delivery;
}
static void worker(void *context)
{
    struct capture *capture = context;
    struct thrift_log_scope scope = thrift_log_begin(THRIFT_LOG_INFO, "worker", 0);
    thrift_log_native("test", WORKER_FAILURE_CODE);
    thrift_log_end(scope, THRIFT_IO);
    capture->worker_ok = thrift_log_last_status() == THRIFT_OK;
}
int main(void)
{
    struct capture capture = {0};
    struct thrift_log_scope outer, inner;
    enum thrift_log_level level = THRIFT_LOG_FATAL;
    const struct thrift_log_event event = {THRIFT_LOG_INFO, THRIFT_LOG_END,
        "application", THRIFT_OK, 0, NULL, 0, NULL};
    CHECK(!thrift_log_enabled(THRIFT_LOG_DEBUG));
    CHECK(thrift_log_enabled(THRIFT_LOG_INFO));
    CHECK(thrift_log_parse_level("warning", &level) == THRIFT_OK);
    CHECK(level == THRIFT_LOG_WARNING);
    CHECK(thrift_log_parse_level("verbose", &level) == THRIFT_INVALID);
    CHECK(level == THRIFT_LOG_WARNING);
    CHECK(thrift_log_parse_level(NULL, &level) == THRIFT_INVALID);
    CHECK(thrift_log_configure(THRIFT_LOG_DEBUG, capture_event, &capture) == THRIFT_OK);
    outer = thrift_log_begin(THRIFT_LOG_INFO, "outer", 0);
    inner = thrift_log_begin(THRIFT_LOG_INFO, "inner", 0);
    CHECK(thrift_log_configure_level("trace") == THRIFT_INVALID);
    thrift_log_native("test", INNER_FAILURE_CODE);
    thrift_log_native("cleanup", CLEANUP_FAILURE_CODE);
    thrift_log_end(inner, THRIFT_IO);
    thrift_log_end(outer, THRIFT_IO);
    CHECK(capture.count == NESTED_EVENT_COUNT);
    CHECK(capture.events[OUTER_BEGIN_INDEX].level == THRIFT_LOG_INFO);
    CHECK(capture.events[INNER_BEGIN_INDEX].level == THRIFT_LOG_DEBUG);
    CHECK(capture.events[INNER_END_INDEX].level == THRIFT_LOG_DEBUG);
    CHECK(capture.events[OUTER_END_INDEX].level == THRIFT_LOG_ERROR);
    CHECK(capture.events[OUTER_END_INDEX].native_code == INNER_FAILURE_CODE);
    CHECK(!strcmp(capture.events[OUTER_END_INDEX].native_domain, "test"));
    CHECK(capture.events[OUTER_BEGIN_INDEX].phase == THRIFT_LOG_BEGIN);
    CHECK(capture.events[OUTER_END_INDEX].phase == THRIFT_LOG_END);
    capture.count = 0;
    CHECK(thrift_log_configure(THRIFT_LOG_ERROR, capture_event, &capture) == THRIFT_OK);
    outer = thrift_log_begin(THRIFT_LOG_INFO, "filtered", 0);
    thrift_log_end(outer, THRIFT_OK);
    CHECK(capture.count == 0);
    CHECK(thrift_log_configure_level("bogus") == THRIFT_INVALID);
    CHECK(!thrift_log_enabled(THRIFT_LOG_INFO));
    CHECK(thrift_log_configure_level("debug") == THRIFT_OK);
    capture.delivery = THRIFT_IO;
    CHECK(thrift_socket_close(NULL) == THRIFT_OK);
    CHECK(thrift_log_last_status() == THRIFT_IO);
    capture.delivery = THRIFT_OK;
    capture.count = 0;
    outer = thrift_log_begin(THRIFT_LOG_INFO, "main", 0);
    thrift_log_native("main", MAIN_FAILURE_CODE);
    CHECK(thrift_test_run_thread(worker, &capture));
    CHECK(capture.worker_ok);
    thrift_log_end(outer, THRIFT_IO);
    CHECK(capture.count == NESTED_EVENT_COUNT);
    CHECK(capture.events[INNER_BEGIN_INDEX].level == THRIFT_LOG_INFO);
    CHECK(capture.events[INNER_END_INDEX].level == THRIFT_LOG_ERROR);
    CHECK(capture.events[INNER_END_INDEX].native_code == WORKER_FAILURE_CODE);
    CHECK(capture.events[OUTER_END_INDEX].native_code == MAIN_FAILURE_CODE);
    capture.count = 0;
    outer = thrift_log_begin(THRIFT_LOG_INFO, "retry", 0);
    thrift_log_native("test", INNER_FAILURE_CODE);
    thrift_log_recovered("retry", "First attempt failed; a second attempt succeeded.");
    thrift_log_end(outer, THRIFT_OK);
    CHECK(capture.events[INNER_BEGIN_INDEX].level == THRIFT_LOG_WARNING);
    CHECK(capture.events[INNER_BEGIN_INDEX].detail != NULL);
    CHECK(capture.events[INNER_END_INDEX].native_domain == NULL);
    thrift_log_write(&event);
    CHECK(thrift_log_last_status() == THRIFT_OK);
    thrift_log_write(NULL);
    CHECK(thrift_log_last_status() == THRIFT_INVALID);
    CHECK(thrift_log_configure(THRIFT_LOG_INFO, NULL, NULL) == THRIFT_OK);
    return EXIT_SUCCESS;
}
