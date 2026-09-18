/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Log public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_LOG_H
#define THRIFT_LOG_H
#include <thrift/c11/thrift.h>

/** @brief Ordered severities; the default threshold is THRIFT_LOG_INFO. */
enum thrift_log_level {
    THRIFT_LOG_TRACE, /**< Fine-grained transfer/iteration detail; disabled by default. */
    THRIFT_LOG_DEBUG, /**< Developer diagnostics and resource operations. */
    THRIFT_LOG_INFO, /**< Major lifecycle events; default threshold. */
    THRIFT_LOG_WARNING, /**< A fault handled by a documented recovery/fallback. */
    THRIFT_LOG_ERROR, /**< Failed requested operation; status is still propagated. */
    THRIFT_LOG_FATAL, /**< Impending process/subsystem termination; logging itself never exits. */
};
/** @brief Stage of an instrumented operation. */
enum thrift_log_phase {
    THRIFT_LOG_BEGIN, /**< Operation started. */
    THRIFT_LOG_END, /**< Operation completed, with success or failure status. */
    THRIFT_LOG_RECOVERY, /**< A fault and successful recovery are reported. */
};
/** @brief Borrowed synchronous event; runtime events never contain payloads or secrets. */
struct thrift_log_event {
    enum thrift_log_level level; /**< Event severity. */
    enum thrift_log_phase phase; /**< Begin, completion or recovery marker. */
    const char *operation; /**< Required borrowed stable operation name. */
    enum thrift_status status; /**< Valid operation status; independent of sink delivery success. */
    size_t size; /**< Byte bound/count relevant to the operation, never payload. */
    const char *native_domain; /**< Optional borrowed error domain such as errno, winsock, curl or zstd. */
    int64_t native_code; /**< Native error value associated with native_domain. */
    const char *detail; /**< Optional borrowed diagnostic/recovery text; exclude secrets. */
};

/**
 * @brief Synchronous event delivery callback.
 * @param[in,out] context Borrowed state installed with thrift_log_configure().
 * @param[in] event Borrowed event valid only during the callback.
 * @return THRIFT_OK on delivery success, or a delivery error observed through thrift_log_last_status().
 * @note Must be thread-safe and must not call runtime/logging functions.
 */
typedef enum thrift_status (*thrift_log_sink)(void *context, const struct thrift_log_event *event);

/**
 * @brief Configure the process-wide threshold and synchronous sink.
 *
 * Default configuration is stderr at info level. A custom sink must be thread-safe
 * and must not call runtime/logging functions. Events are borrowed during callbacks.
 * @note Requires process-wide exclusive access: configure before workers start or
 * after all runtime users have joined. Do not reconfigure from a callback/active scope.
 *
 * @param[in] level Minimum severity to deliver.
 * @param[in] sink Callback, or NULL to select stderr.
 * @param[in,out] context Borrowed sink state retained until reconfiguration; must be NULL for stderr.
 * @return THRIFT_OK or THRIFT_INVALID; rejected configuration leaves the old settings intact.
 */
enum thrift_status thrift_log_configure(enum thrift_log_level level,
                                               thrift_log_sink sink, void *context);

/**
 * @brief Select the threshold from CLI override, environment or default.
 *
 * Precedence: non-NULL override, THRIFT_LOG_LEVEL, info. Preserves the current sink.
 * No other runtime API implicitly reads this environment variable.
 * @note Same exclusive configuration requirements as thrift_log_configure().
 *
 * @param[in] override_level Lowercase level string, or NULL to consult THRIFT_LOG_LEVEL.
 * @return THRIFT_OK, or THRIFT_INVALID without changing configuration.
 */
enum thrift_status thrift_log_configure_level(const char *override_level);

/**
 * @brief Parse a lowercase severity name.
 *
 * @note Pure, reentrant and thread-safe.
 *
 * @param[in] name trace, debug, info, warning, error or fatal.
 * @param[out] level Required destination, assigned only on success.
 * @return THRIFT_OK or THRIFT_INVALID.
 */
enum thrift_status thrift_log_parse_level(const char *name, enum thrift_log_level *level);
/**
 * @brief Return the canonical severity name.
 *
 * @note Pure, reentrant and thread-safe.
 *
 * @param[in] level Severity to describe.
 * @return Borrowed static lowercase string, or "invalid" for an unknown value.
 */
const char *thrift_log_level_name(enum thrift_log_level level);

/**
 * @brief Check the active threshold before expensive event preparation.
 *
 * @note Safe for concurrent reads after configuration; must not race reconfiguration.
 *
 * @param[in] level Severity to test.
 * @return True if the valid severity meets the threshold; otherwise false.
 */
bool thrift_log_enabled(enum thrift_log_level level);

/**
 * @brief Inspect the calling thread's most recent sink delivery result.
 *
 * Filtered events do not update this status. Delivery failures never replace an
 * operation's status. A later successful delivery can overwrite an earlier failure.
 * @note Per-thread state; query on the worker whose delivery is being monitored.
 * @return THRIFT_OK initially, otherwise the last delivery status or THRIFT_INVALID for an invalid event.
 */
enum thrift_status thrift_log_last_status(void);

/**
 * @brief Deliver a validated application event through the configured sink.
 *
 * Invalid events record THRIFT_INVALID in thrift_log_last_status(). Disabled events
 * are filtered. Callers must exclude secrets and sanitize external text themselves.
 * @note Concurrent calls are safe after configuration with a thread-safe sink.
 * The sink must not reenter runtime/logging functions; do not race reconfiguration.
 *
 * @param[in] event Borrowed event and strings, valid until this call returns.
 */
void thrift_log_write(const struct thrift_log_event *event);
#endif
