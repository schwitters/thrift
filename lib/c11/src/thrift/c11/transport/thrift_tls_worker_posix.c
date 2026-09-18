/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <pthread.h>
#include <stdlib.h>
/* mutex protects job, busy, done, stop. The processor runs without the lock.
 * A job stays owned by the reactor until take() or destroy() joins its worker. */
struct thrift_tls_worker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    void (*run)(void *);
    void *job;
    bool busy, done, stop;
};
static void check_thread(int error)
{
    /* These are internal synchronization invariants, not peer-controlled input. */
    if (error) { thrift_log_native("pthread", error); abort(); }
}
static void *work(void *argument)
{
    struct thrift_tls_worker *worker = argument;
    check_thread(pthread_mutex_lock(&worker->mutex));
    for (;;) {
        while (!worker->stop && (!worker->busy || worker->done))
            check_thread(pthread_cond_wait(&worker->condition, &worker->mutex));
        if (worker->stop) break;
        check_thread(pthread_mutex_unlock(&worker->mutex));
        worker->run(worker->job);
        check_thread(pthread_mutex_lock(&worker->mutex));
        worker->done = true;
    }
    check_thread(pthread_mutex_unlock(&worker->mutex));
    return NULL;
}
enum thrift_status thrift_tls_worker_create(void (*run)(void *), struct thrift_tls_worker **out)
{
    struct thrift_tls_worker *worker = calloc(1, sizeof(*worker));
    int error;
    *out = NULL;
    if (!worker) return THRIFT_NOMEM;
    worker->run = run;
    error = pthread_mutex_init(&worker->mutex, NULL);
    if (error) { thrift_log_native("pthread_mutex_init", error); free(worker); return THRIFT_IO; }
    error = pthread_cond_init(&worker->condition, NULL);
    if (error) { thrift_log_native("pthread_cond_init", error); check_thread(pthread_mutex_destroy(&worker->mutex)); free(worker); return THRIFT_IO; }
    error = pthread_create(&worker->thread, NULL, work, worker);
    if (error) {
        thrift_log_native("pthread_create", error);
        check_thread(pthread_cond_destroy(&worker->condition));
        check_thread(pthread_mutex_destroy(&worker->mutex)); free(worker); return THRIFT_IO;
    }
    *out = worker;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_worker_submit(struct thrift_tls_worker *worker, void *job)
{
    enum thrift_status status = THRIFT_AGAIN;
    check_thread(pthread_mutex_lock(&worker->mutex));
    if (!worker->busy) {
        worker->job = job; worker->busy = true; worker->done = false;
        check_thread(pthread_cond_signal(&worker->condition));
        status = THRIFT_OK;
    }
    check_thread(pthread_mutex_unlock(&worker->mutex));
    return status;
}
bool thrift_tls_worker_take(struct thrift_tls_worker *worker, void **job)
{
    bool done;
    check_thread(pthread_mutex_lock(&worker->mutex));
    done = worker->done;
    if (done) { *job = worker->job; worker->busy = worker->done = false; worker->job = NULL; }
    check_thread(pthread_mutex_unlock(&worker->mutex));
    return done;
}
void thrift_tls_worker_destroy(struct thrift_tls_worker *worker)
{
    if (!worker) return;
    check_thread(pthread_mutex_lock(&worker->mutex));
    worker->stop = true;
    check_thread(pthread_cond_signal(&worker->condition));
    check_thread(pthread_mutex_unlock(&worker->mutex));
    check_thread(pthread_join(worker->thread, NULL));
    check_thread(pthread_cond_destroy(&worker->condition));
    check_thread(pthread_mutex_destroy(&worker->mutex));
    free(worker);
}
