/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Processor public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_PROCESSOR_H
#define THRIFT_PROCESSOR_H
#include <thrift/c11/protocol/thrift_protocol.h>

/** @brief Immutable generated RPC method descriptor.
 * The descriptor and its referenced tables/strings are borrowed for each call. */
struct thrift_method {
    const char *name; /**< Borrowed unprefixed wire method name. */
    const struct thrift_record *args; /**< Borrowed generated argument record descriptor. */
    const struct thrift_record *result; /**< Borrowed generated result record descriptor. */
    bool oneway; /**< Send without reading/writing a reply. */
    bool returns_value; /**< Whether a successful reply requires a success field or declared exception. */
    /** @brief Generated synchronous adapter. Arguments are borrowed; the initialized result owns
     * handler-created fields. Return THRIFT_OK for success/declared exception; other statuses
     * become application failures. Do not retain args/result.
     */
    enum thrift_status (*invoke)(void *handler, const void *args, void *result);
};

/** @brief Borrowed service registration returned by generated service helpers.
 * The name may be replaced with a borrowed alias before registration.
 * Synchronize mutable handler state if registrations are shared across threads. */
struct thrift_service {
    const char *name; /**< Borrowed unique registration name; nonempty and without a colon. */
    const struct thrift_method *methods; /**< Borrowed immutable method table, or NULL for no methods. */
    size_t method_count; /**< Number of method entries. */
    void *handler; /**< Borrowed non-NULL generated handler table passed to invoke. */
};

/**
 * @brief Send one synchronous RPC and decode its reply when applicable.
 *
 * Writes arguments and flushes when available. Oneway returns after sending, without
 * reading a reply. Inspect generated has_success/has_exception flags on success.
 * Unlike thrift_record_read(), failure after result validation may leave a newly
 * decoded result in the destination; always clear it. Protocol/transport failures
 * require discarding the connection. No argument or handler ownership is transferred.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Initialized protocol; exclusively held for the complete call.
 * @param[in] method Trusted generated method descriptor.
 * @param[in] sequence Caller-selected sequence identifier, checked in replies.
 * @param[in] args Valid argument record matching method->args.
 * @param[in,out] result Initialized result record; may be NULL for oneway methods.
 * @return THRIFT_OK on successful RPC (including a declared exception); THRIFT_REMOTE for an application exception, THRIFT_MISSING_RESULT for absent nonvoid results, or another runtime error.
 */
enum thrift_status thrift_client_call(struct thrift_protocol *protocol,
                                             const struct thrift_method *method,
                                             int32_t sequence, const void *args, void *result);

/**
 * @brief Read and dispatch exactly one request.
 *
 * Unknown methods and handler failures can be encoded as application exceptions;
 * successfully sending such a reply returns THRIFT_OK. Oneway requests receive no
 * reply. The caller owns accepting connections and scheduling further requests.
 * Callbacks run synchronously and must not retain borrowed argument/result storage.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Initialized protocol for request and reply I/O.
 * @param[in] methods Borrowed immutable generated method table; NULL if count is zero.
 * @param[in] count Number of table entries.
 * @param[in,out] handler Borrowed non-NULL handler passed to the generated invoke callback.
 * @return THRIFT_OK when handling and any reply succeed; otherwise a runtime error.
 */
enum thrift_status thrift_process(struct thrift_protocol *protocol,
                                         const struct thrift_method *methods,
                                         size_t count, void *handler);
#endif
