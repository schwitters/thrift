/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Common status codes, owned values and immutable type descriptors.
 * @par Ownership and synchronization
 * Objects and transports are independently reentrant. Concurrent access to the
 * same mutable object requires caller synchronization. Descriptors are immutable.
 * Initialize owned values to zero before use; clear releases all descendants.
 * Owned pointers must be free()-compatible, unshared and acyclic.
 * Discard connections after protocol/transport failures: stream positions and
 * budgets are not rolled back. Per-function contracts describe output handling.
 */
#ifndef THRIFT_THRIFT_H
#define THRIFT_THRIFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Operation status; errors are returned independently of logging. */
enum thrift_status {
    THRIFT_OK = 0, /**< Operation succeeded. */
    THRIFT_INVALID, /**< Invalid argument or object state. */
    THRIFT_NOMEM, /**< Allocation failed. */
    THRIFT_IO, /**< Transport or platform I/O failure. */
    THRIFT_TIMEOUT, /**< Transport timeout. */
    THRIFT_EOF, /**< Insufficient input or unexpected stream end. */
    THRIFT_PROTOCOL, /**< Malformed or inconsistent protocol message. */
    THRIFT_LIMIT, /**< Resource budget or representable size exceeded. */
    THRIFT_REQUIRED, /**< Required field missing or received with an incompatible wire type. */
    THRIFT_REMOTE, /**< Remote application exception or unsuccessful HTTP status. */
    THRIFT_MISSING_RESULT, /**< Nonvoid RPC returned neither success nor a declared exception. */
    THRIFT_AGAIN, /**< Nonblocking operation needs another readiness event; state is preserved. */
    THRIFT_NULL_CLIENT, /**< Client pointer is NULL. */
    THRIFT_NULL_PROTOCOL, /**< Protocol pointer is NULL. */
    THRIFT_PROTOCOL_UNINITIALIZED, /**< Protocol has no initialized operations. */
    THRIFT_NULL_ARGS, /**< RPC argument record pointer is NULL. */
    THRIFT_NULL_RESULT, /**< Required RPC result record pointer is NULL. */
    THRIFT_INVALID_SEQUENCE, /**< Automatic client sequence counter is outside its valid range. */
    THRIFT_NULL_HANDLER, /**< Server handler pointer is NULL. */
    THRIFT_NULL_METHOD, /**< Required method descriptor or method table is NULL. */
    THRIFT_MISSING_CALLBACK, /**< The registered method has no callback. */
    THRIFT_NOT_IMPLEMENTED, /**< The server method has not been implemented. */
    THRIFT_NULL_VALUE, /**< Destination value pointer is NULL. */
    THRIFT_NULL_FIELD, /**< Field value pointer is NULL. */
    THRIFT_NULL_DATA, /**< Nonempty input has no data storage. */
    THRIFT_INVALID_CAPACITY, /**< Container size exceeds its nonzero capacity. */
    THRIFT_NULL_MAP_KEYS, /**< Nonempty map has no key storage. */
    THRIFT_NULL_MAP_VALUES, /**< Nonempty map has no value storage. */
    THRIFT_INVALID_OWNERSHIP, /**< Direct self-ownership or shared destination storage was detected. */
    THRIFT_OUTPUT_NOT_EMPTY, /**< Allocation output already contains an object. */
    THRIFT_STATUS_COUNT, /**< Status range boundary; not an operation result. */
};

/** @brief Thrift wire type identifiers shared by supported protocols. */
enum thrift_wire {
    THRIFT_STOP = 0, /**< End of record fields. */
    THRIFT_BOOL = 2, /**< Boolean. */
    THRIFT_BYTE = 3, /**< Signed 8-bit integer. */
    THRIFT_DOUBLE = 4, /**< IEEE 754 binary64. */
    THRIFT_I16 = 6, /**< Signed 16-bit integer. */
    THRIFT_I32 = 8, /**< Signed 32-bit integer. */
    THRIFT_I64 = 10, /**< Signed 64-bit integer. */
    THRIFT_STRING = 11, /**< Length-delimited string or binary data. */
    THRIFT_STRUCT = 12, /**< Struct, union or exception record. */
    THRIFT_MAP = 13, /**< Key/value container. */
    THRIFT_SET = 14, /**< Set container. */
    THRIFT_LIST = 15, /**< List container. */
    THRIFT_UUID = 16, /**< 16-byte UUID. */
};

/** @brief Owned string/binary bytes; initialize to zero before use.
 * Embedded NUL bytes are valid; size is authoritative. Release via
 * thrift_value_clear() with thrift_type_string or the generated clear function. */
struct thrift_bytes {
    size_t size; /**< Number of payload bytes, excluding any trailing NUL. */
    uint8_t *data; /**< Owned free()-compatible storage, or NULL for empty data. */
};
enum {
    THRIFT_UUID_SIZE = 16, /**< UUID size in bytes. */
};
/** @brief UUID bytes in Thrift wire order; contains no dynamic allocation. */
struct thrift_uuid {
    uint8_t data[THRIFT_UUID_SIZE]; /**< The 16 UUID bytes. */
};

/** @brief Owned list or set storage; element representation follows its descriptor.
 * Struct elements are owned pointers. Sizes must fit INT32_MAX. The caller
 * maintains set uniqueness; runtime storage does not deduplicate elements. */
struct thrift_list {
    size_t size; /**< Number of initialized elements. */
    void *data; /**< Owned contiguous native elements, or NULL when empty. */
    size_t capacity; /**< Allocated slots; zero also accepts legacy storage sized by size. */
};
/** @brief Owned parallel key/value arrays with descriptor-defined representations.
 * Size must fit INT32_MAX. The caller maintains key uniqueness. */
struct thrift_map {
    size_t size; /**< Number of initialized key/value pairs. */
    void *keys; /**< Owned contiguous native keys. */
    void *values; /**< Owned contiguous native values. */
    size_t capacity; /**< Allocated slots; zero also accepts legacy storage sized by size. */
};

struct thrift_type;
/** @brief Immutable field metadata emitted by the generator; trusted by the runtime. */
struct thrift_field {
    int16_t id; /**< Signed wire field identifier. */
    size_t offset; /**< Byte offset of the native value within the record. */
    size_t present_offset; /**< Byte offset of the bool presence flag. */
    bool required; /**< Whether decoding must find this field with the correct wire type. */
    bool optional; /**< Whether serialization depends on the presence flag. */
    const struct thrift_type *type; /**< Borrowed immutable descriptor for the field representation. */
};
/** @brief Immutable layout and default-initialization metadata for a generated record.
 * Descriptor graphs must outlive every object/protocol operation using them. */
struct thrift_record {
    size_t size; /**< Native record storage size, not pointer size. */
    size_t field_count; /**< Number of field descriptors. */
    const struct thrift_field *fields; /**< Borrowed field array; NULL for an empty record. */
    bool exclusive; /**< At most one field may be present (union or RPC result). */
    /** @brief Optional generated callback initializing empty storage; charges allocations to the
     * nullable budget and returns a thrift_status.
     */
    enum thrift_status (*defaults)(void *value, size_t *allocation_budget);
};
/** @brief Immutable native representation and wire type descriptor.
 * Struct values occupy pointer-sized slots; the pointed-to record has its own size.
 * Applications should use generated descriptors, not invent inconsistent layouts. */
struct thrift_type {
    enum thrift_wire wire; /**< Wire representation. */
    size_t size; /**< Native slot size in bytes, including pointer size for struct values. */
    const struct thrift_record *record; /**< Record layout for THRIFT_STRUCT; otherwise NULL. */
    const struct thrift_type *key; /**< Map key descriptor; otherwise NULL. */
    const struct thrift_type *element; /**< List/set element or map value descriptor; otherwise NULL. */
};
/** @brief Immutable built-in bool descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_bool;
/** @brief Immutable built-in byte descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_byte;
/** @brief Immutable built-in i16 descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_i16;
/** @brief Immutable built-in i32 descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_i32;
/** @brief Immutable built-in i64 descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_i64;
/** @brief Immutable built-in double descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_double;
/** @brief Immutable built-in string descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_string;
/** @brief Immutable built-in uuid descriptor; safe for concurrent reads. */
extern const struct thrift_type thrift_type_uuid;

/**
 * @brief Describe a status code.
 *
 * @note Reentrant and thread-safe; the returned string must not be freed.
 *
 * @param[in] status Status to describe.
 * @return Borrowed static English text; unknown codes return "unknown status".
 */
const char *thrift_status_string(enum thrift_status status);
/**
 * @brief Replace a byte string with an owned copy.
 *
 * On failure the destination is unchanged. Nonempty copies have a trailing NUL
 * not included in size; embedded NUL bytes remain valid data. Empty copies release
 * old storage. Source bytes may overlap the old destination.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] value Zero-initialized or valid owned byte string to replace.
 * @param[in] data Bytes to copy; may be NULL only if size is zero.
 * @param[in] size Byte count, at most INT32_MAX.
 * @return THRIFT_OK on success; THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for size/budget overflow, or THRIFT_NOMEM on allocation failure.
 */
enum thrift_status thrift_bytes_set(struct thrift_bytes *value,
                                           const void *data, size_t size);
/** @brief Copy a NUL-terminated string into owned bytes (excluding its terminator).
 * NULL arguments are invalid. Failure preserves the destination; overlap is safe.
 * @param[in,out] value Zero-initialized or valid owned bytes.
 * @param[in] text NUL-terminated source string.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT or THRIFT_NOMEM.
 */
enum thrift_status thrift_bytes_set_cstr(struct thrift_bytes *value, const char *text);

/** @brief Reserve storage without changing the number of initialized elements.
 * Preserves values; invalidates element pointers when storage grows. New slots
 * are zeroed. Capacity zero accepts manually allocated storage of size slots.
 * Failure preserves the list and budget. Successful growth charges the complete
 * new allocation, not just the capacity difference; free does not refund budget.
 * @param[in,out] list Zero-initialized or valid owned list/set.
 * @param[in] count Minimum capacity, at most INT32_MAX.
 * @param[in] element_size Native slot size (descriptor->element->size), nonzero.
 * @param[in,out] budget Remaining allocation bytes, or NULL for no budget.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT or THRIFT_NOMEM.
 */
enum thrift_status thrift_list_reserve(struct thrift_list *list, size_t count,
    size_t element_size, size_t *budget);

/** @brief Append one native element, growing storage geometrically as needed.
 * Copies the slot, not its descendants. On success ownership of descendants is
 * transferred to the list: reset the source without clearing it. Owned elements
 * must be unshared and acyclic; only scalar elements may alias existing slots.
 * Failure preserves the list, source and budget. Set uniqueness is caller-owned.
 * @param[in,out] list Zero-initialized or valid owned list/set.
 * @param[in] element Address of a native slot (address of pointer for structs).
 * @param[in] element_size Same nonzero size used to reserve/populate this list.
 * @param[in,out] budget Remaining bytes, or NULL; growth charges the new allocation.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_LIMIT or THRIFT_NOMEM.
 */
enum thrift_status thrift_list_append(struct thrift_list *list, const void *element,
    size_t element_size, size_t *budget);

/** @brief Reserve storage without changing the number of initialized pairs.
 * Preserves values; invalidates key/value pointers when storage grows. New slots
 * are zeroed. Capacity zero accepts manually allocated storage of size slots.
 * Failure preserves the map and budget. Successful growth charges the complete
 * new key and value allocations, not just the capacity difference; free does
 * not refund budget.
 * @param[in,out] map Zero-initialized or valid owned map.
 * @param[in] count Minimum capacity, at most INT32_MAX.
 * @param[in] key_size Native key slot size (descriptor->key->size), nonzero.
 * @param[in] value_size Native value slot size (descriptor->element->size), nonzero.
 * @param[in,out] budget Remaining allocation bytes, or NULL for no budget.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NULL_MAP_KEYS, THRIFT_NULL_MAP_VALUES, THRIFT_LIMIT or THRIFT_NOMEM.
 */
enum thrift_status thrift_map_reserve(struct thrift_map *map, size_t count,
    size_t key_size, size_t value_size, size_t *budget);

/** @brief Append one native key/value pair, growing storage geometrically as needed.
 * Copies both slots, not their descendants. On success ownership of descendants is
 * transferred to the map: reset the sources without clearing them. Owned pairs
 * must be unshared and acyclic; only scalar keys/values may alias existing slots.
 * Failure preserves the map, sources and budget. Key uniqueness is caller-owned;
 * this never looks up or replaces an existing key.
 * @param[in,out] map Zero-initialized or valid owned map.
 * @param[in] key Address of a native key slot (address of pointer for structs).
 * @param[in] value Address of a native value slot (address of pointer for structs).
 * @param[in] key_size Same nonzero size used to reserve/populate this map's keys.
 * @param[in] value_size Same nonzero size used to reserve/populate this map's values.
 * @param[in,out] budget Remaining bytes, or NULL; growth charges the new allocations.
 * @return THRIFT_OK, THRIFT_INVALID, THRIFT_NULL_MAP_KEYS, THRIFT_NULL_MAP_VALUES, THRIFT_LIMIT or THRIFT_NOMEM.
 */
enum thrift_status thrift_map_append(struct thrift_map *map, const void *key, const void *value,
    size_t key_size, size_t value_size, size_t *budget);

/**
 * @brief Release an owned value recursively and zero its storage.
 *
 * For a struct descriptor, value points to a struct pointer, not to the struct.
 * All owned allocations must be free()-compatible, unshared and acyclic. Repeated
 * clears are safe. The outer value storage itself remains owned by the caller.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] type Trusted immutable descriptor; NULL is a no-op.
 * @param[in,out] value Address of the native value; NULL is a no-op.
 */
void thrift_value_clear(const struct thrift_type *type, void *value);
/**
 * @brief Release record fields and zero the record without freeing its outer storage.
 *
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] record Trusted immutable record descriptor; NULL is a no-op.
 * @param[in,out] value Address of a valid or zero-initialized record; NULL is a no-op.
 */
void thrift_record_clear(const struct thrift_record *record, void *value);

/**
 * @brief Zero an empty record and apply generated defaults.
 *
 * @pre Clear any previous contents first: initialization does not release them.
 * Default initialization may allocate. On failure, partial fields are cleared and
 * the record is zeroed; any consumed allocation budget is not refunded.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] record Trusted immutable record descriptor.
 * @param[out] value Empty record storage of at least record->size bytes.
 * @return THRIFT_OK on success; THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for size/budget overflow, or THRIFT_NOMEM on allocation failure.
 */
enum thrift_status thrift_record_init(const struct thrift_record *record, void *value);

/**
 * @brief Allocate zero-filled storage with overflow and budget checks.
 *
 * The caller owns successful allocations and releases them with free(). The budget
 * is decreased only on success; clearing allocations does not refund it.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] count Element count; zero succeeds without allocating.
 * @param[in] size Nonzero element size in bytes.
 * @param[in,out] budget Remaining allocation bytes, or NULL for no budget.
 * @param[out] out Required destination; set to NULL before allocation and on failure.
 * @return THRIFT_OK on success; THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for size/budget overflow, or THRIFT_NOMEM on allocation failure.
 */
enum thrift_status thrift_alloc(size_t count, size_t size, size_t *budget, void **out);
/**
 * @brief Replace a byte string with an owned copy.
 *
 * On failure the destination is unchanged. Nonempty copies have a trailing NUL
 * not included in size; embedded NUL bytes remain valid data. Empty copies release
 * old storage. Source bytes may overlap the old destination.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in,out] value Zero-initialized or valid owned byte string to replace.
 * @param[in] data Bytes to copy; may be NULL only if size is zero.
 * @param[in] size Byte count, at most INT32_MAX.
 * @param[in,out] budget Remaining allocation bytes, or NULL; nonempty copies charge size + 1.
 * @return THRIFT_OK on success; THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for size/budget overflow, or THRIFT_NOMEM on allocation failure.
 */
enum thrift_status thrift_bytes_set_limited(struct thrift_bytes *value,
    const void *data, size_t size, size_t *budget);
/**
 * @brief Zero an empty record and apply generated defaults.
 *
 * @pre Clear any previous contents first: initialization does not release them.
 * Default initialization may allocate. On failure, partial fields are cleared and
 * the record is zeroed; any consumed allocation budget is not refunded.
 * @note Thread safety: synchronize access to shared mutable objects; independent objects may be used concurrently.
 *
 * @param[in] record Trusted immutable record descriptor.
 * @param[out] value Empty record storage of at least record->size bytes.
 * @param[in,out] budget Remaining allocation bytes, or NULL for no budget.
 * @return THRIFT_OK on success; THRIFT_INVALID for invalid arguments, THRIFT_LIMIT for size/budget overflow, or THRIFT_NOMEM on allocation failure.
 */
enum thrift_status thrift_record_init_limited(const struct thrift_record *record,
    void *value, size_t *budget);
#endif
