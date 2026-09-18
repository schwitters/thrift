/* SPDX-License-Identifier: Apache-2.0 */
#ifndef THRIFT_TEST_LOGGING_THREAD_H
#define THRIFT_TEST_LOGGING_THREAD_H
#include <stdbool.h>
/* Start one worker and join it before returning. Callback/context are borrowed. */
bool thrift_test_run_thread(void (*callback)(void *), void *context);
#endif
