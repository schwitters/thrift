/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift_tls_internal.h"
#include "../thrift_log_internal.h"
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <errno.h>
/* mutex protects job, busy, done, stop. The processor runs without the lock.
 * A job stays owned by the reactor until take() or destroy() joins its worker. */
struct thrift_tls_worker {
    HANDLE thread;
    SRWLOCK mutex;
    CONDITION_VARIABLE condition;
    void (*run)(void *);
    void *job;
    bool busy, done, stop;
};
static unsigned __stdcall work(void *argument)
{
    struct thrift_tls_worker *worker = argument;
    AcquireSRWLockExclusive(&worker->mutex);
    for (;;) {
        while (!worker->stop && (!worker->busy || worker->done))
            if (!SleepConditionVariableSRW(&worker->condition, &worker->mutex, INFINITE, 0)) abort();
        if (worker->stop) break;
        ReleaseSRWLockExclusive(&worker->mutex);
        worker->run(worker->job);
        AcquireSRWLockExclusive(&worker->mutex);
        worker->done = true;
    }
    ReleaseSRWLockExclusive(&worker->mutex);
    return 0;
}
enum thrift_status thrift_tls_worker_create(void (*run)(void *), struct thrift_tls_worker **out)
{
    struct thrift_tls_worker *worker = calloc(1, sizeof(*worker));
    *out = NULL;
    if (!worker) return THRIFT_NOMEM;
    worker->run = run;
    InitializeSRWLock(&worker->mutex);
    InitializeConditionVariable(&worker->condition);
    worker->thread = (HANDLE)_beginthreadex(NULL, 0, work, worker, 0, NULL);
    if (!worker->thread) { int saved = errno; thrift_log_native("_beginthreadex.errno", saved); free(worker); return THRIFT_IO; }
    *out = worker;
    return THRIFT_OK;
}
enum thrift_status thrift_tls_worker_submit(struct thrift_tls_worker *worker, void *job)
{
    enum thrift_status status = THRIFT_AGAIN;
    AcquireSRWLockExclusive(&worker->mutex);
    if (!worker->busy) {
        worker->job = job; worker->busy = true; worker->done = false;
        WakeConditionVariable(&worker->condition);
        status = THRIFT_OK;
    }
    ReleaseSRWLockExclusive(&worker->mutex);
    return status;
}
bool thrift_tls_worker_take(struct thrift_tls_worker *worker, void **job)
{
    bool done;
    AcquireSRWLockExclusive(&worker->mutex);
    done = worker->done;
    if (done) { *job = worker->job; worker->busy = worker->done = false; worker->job = NULL; }
    ReleaseSRWLockExclusive(&worker->mutex);
    return done;
}
void thrift_tls_worker_destroy(struct thrift_tls_worker *worker)
{
    if (!worker) return;
    AcquireSRWLockExclusive(&worker->mutex);
    worker->stop = true;
    WakeConditionVariable(&worker->condition);
    ReleaseSRWLockExclusive(&worker->mutex);
    if (WaitForSingleObject(worker->thread, INFINITE) != WAIT_OBJECT_0) abort();
    if (!CloseHandle(worker->thread)) abort();
    free(worker);
}
