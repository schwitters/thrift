/* SPDX-License-Identifier: Apache-2.0 */
#include "test_logging_thread.h"
#include <pthread.h>
struct worker { void (*callback)(void *); void *context; };
static void *run(void *context)
{
    struct worker *worker = context;
    worker->callback(worker->context);
    return NULL;
}
bool thrift_test_run_thread(void (*callback)(void *), void *context)
{
    struct worker worker = {callback, context};
    pthread_t thread;
    if (pthread_create(&thread, NULL, run, &worker) != 0)
        return false;
    return pthread_join(thread, NULL) == 0;
}
