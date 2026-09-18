/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Protocol public API.
 * @details See thrift.h for the shared ownership and synchronization contract.
 */
#ifndef THRIFT_PROTOCOL_H
#define THRIFT_PROTOCOL_H
#include <thrift/c11/transport/thrift_transport.h>

/** @brief Supported serialization formats. */
enum thrift_protocol_kind {
    THRIFT_BINARY, /**< Versioned Binary protocol. */
    THRIFT_COMPACT, /**< Compact protocol version 1. */
};
struct thrift_protocol_ops;

/** @brief Per-message resource ceilings; initialization installs finite defaults.
 * Set before operations and call thrift_protocol_reset() after changing them. */
struct thrift_limits {
    size_t max_bytes; /**< Wire-byte budget per message (default 16 MiB). */
    size_t max_allocation; /**< Cumulative allocation-byte budget per message (default 32 MiB). */
    size_t max_container; /**< Maximum element count per container (default 1048576). */
    unsigned max_depth; /**< Maximum traversal depth (default 64). */
};
/** @brief Caller-owned protocol state borrowing its transport context.
 * No destructor is needed. Each operation requires exclusive access to this
 * object; independently backed protocols can be used concurrently. */
struct thrift_protocol {
    const struct thrift_protocol_ops *ops; /**< Implementation table selected by initialization; do not modify. */
    enum thrift_protocol_kind kind; /**< Selected serialization format; do not modify directly. */
    const char *service_name; /**< Borrowed multiplexed client name or NULL; keep alive during use. */
    struct thrift_transport transport; /**< Copied callback table with borrowed context. */
    struct thrift_limits limits; /**< Caller-configurable resource ceilings. */
    size_t remaining_bytes; /**< Internal remaining wire budget; reset with thrift_protocol_reset(). */
    size_t remaining_allocation; /**< Internal cumulative allocation budget; reset at message boundaries. */
};

/**
 * @brief Initialize versioned Binary.
 *
 * At least one of the read/write callbacks must be present. The backing transport
 * context must outlive all uses of the protocol. Installs default limits: 16 MiB
 * wire bytes, 32 MiB allocation, 1048576 container elements and depth 64.
 * No I/O or allocation is performed.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[out] protocol Caller-owned protocol storage.
 * @param[in] transport Transport view copied by value; its context remains borrowed.
 * @return THRIFT_OK, or THRIFT_INVALID for invalid arguments.
 */
enum thrift_status thrift_protocol_init(struct thrift_protocol *protocol,
                                               struct thrift_transport transport);

/**
 * @brief Initialize the selected Binary or Compact protocol.
 *
 * At least one of the read/write callbacks must be present. The backing transport
 * context must outlive all uses of the protocol. Installs default limits: 16 MiB
 * wire bytes, 32 MiB allocation, 1048576 container elements and depth 64.
 * No I/O or allocation is performed.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[out] protocol Caller-owned protocol storage.
 * @param[in] transport Transport view copied by value; its context remains borrowed.
 * @param[in] kind THRIFT_BINARY or THRIFT_COMPACT.
 * @return THRIFT_OK, or THRIFT_INVALID for invalid arguments.
 */
enum thrift_status thrift_protocol_init_kind(struct thrift_protocol *protocol,
    struct thrift_transport transport, enum thrift_protocol_kind kind);

/**
 * @brief Restore byte and allocation budgets from current limits.
 *
 * Call at message boundaries, including after changing limits. RPC message handling
 * resets budgets automatically; standalone record operations do not.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Protocol to reset; NULL is a no-op.
 */
void thrift_protocol_reset(struct thrift_protocol *protocol);

/**
 * @brief Decode a record transactionally into initialized storage.
 *
 * Replaces and releases the old destination only after successful decoding. Failure
 * leaves the destination unchanged, but does not roll back stream position or budgets.
 * Unknown fields are skipped; required fields must appear with the correct wire type.
 * Discard the connection after decoding/transport failure.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Initialized protocol with a readable transport.
 * @param[in] record Trusted immutable descriptor matching the destination.
 * @param[in,out] value Zero-initialized or valid owned destination record.
 * @return THRIFT_OK, or an argument, transport, protocol, required-field, allocation or resource-limit error.
 */
enum thrift_status thrift_record_read(struct thrift_protocol *protocol,
                                             const struct thrift_record *record, void *value);
/**
 * @brief Serialize a record using its generated descriptor.
 *
 * Does not flush the transport. Failure can leave a partial message on the stream;
 * output and budgets are not rolled back. Optional fields follow their has_* flags.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] protocol Initialized protocol with a writable transport.
 * @param[in] record Trusted immutable descriptor matching the value.
 * @param[in] value Valid record and owned descendants; borrowed for this call.
 * @return THRIFT_OK, or an argument, transport, protocol or resource-limit error.
 */
enum thrift_status thrift_record_write(struct thrift_protocol *protocol,
                                              const struct thrift_record *record,
                                              const void *value);

#endif
