# Apache Thrift C11

This module adds `thrift --gen c11` and a dependency-free C11 runtime. The existing
Thrift compiler remains a C++ program; its new backend generates C11 source and
headers. The runtime uses the C standard library and native TCP sockets on Linux
and Windows (Winsock), plus filesystem Unix-domain sockets on Linux. The core
does not require GLib or a C++ runtime. Optional
Zstandard, HTTP and WebSocket transports use libzstd, libcurl and CivetWeb.
Optional nonblocking TLS uses OpenSSL on Linux and native Schannel/CNG on Windows.

## Source layout

The layout follows `lib/c_glib/src/thrift/c_glib`:

```text
src/thrift/c11/
  thrift.h, thrift.c                types, ownership, allocation, status codes
  thrift_log.h, thrift_log.c        central logging API and filtering
  thrift_log_posix.c                per-thread logging state on Linux
  thrift_log_windows.c              per-thread logging state on Windows
  processor/                       RPC dispatch and multiplexed service routing
  protocol/                        Binary, Compact, Multiplexed, shared limits
  server/                          synchronous TCP helper, TLS reactor, CivetWeb HTTP/WebSocket
  transport/                       memory, TCP/TLS, Unix sockets, Zstandard, HTTP, WebSocket
    thrift_socket_posix.c          Linux implementation
    thrift_socket_windows.c        Windows implementation
```

CMake selects exactly one socket implementation. Public interfaces contain no
platform handles. Each Windows socket owns a Winsock initialization reference.

## Implemented scope

- Fixed-width integers, boolean, IEEE binary64, length-delimited string/binary,
  UUID, enums, typedefs, structs, recursive structs, unions and exceptions.
- Lists, sets and maps, including nested containers and struct elements.
- Owned constants, field defaults, optional presence and required-field checks.
  Explicit defaults set `has_*`, matching C++ generation; clear that flag to omit
  an optional field deliberately. Regenerate bindings after updating the compiler.
- Included IDL files and service inheritance.
- Public fields and constant accessors retain declared typedef names. Alias
  definitions use resolved types so forward alias chains remain valid C. Field IDs
  outside the signed 16-bit wire range are rejected by the C11 generator.
- Synchronous clients, handler tables, server dispatch, declared exceptions,
  application exceptions, sequence validation, void and oneway methods.
- Versioned Binary and Compact v1 protocols, multiplexed clients/processors with
  explicit service registration and optional default-service routing.
- Fixed-capacity memory transport, blocking IPv4/IPv6 TCP, per-read/write timeouts
  and a synchronous server helper for either serialization protocol.
- POSIX-only filesystem Unix-domain stream sockets using the same transport and
  synchronous server APIs; no Windows implementation.
- Optional Zstandard transport, libcurl HTTP/HTTPS client and CivetWeb HTTP
  server with bounded request/response buffering.
- Optional libcurl WebSocket client (ws/wss) and CivetWeb WebSocket server,
  with continuation frames, Ping/Pong/Close and synchronous oneway/RPC support.
- Optional nonblocking TLS client/listener sockets and a framed TLS server with a
  bounded worker pool, plus a synchronous framed adapter for generated clients.
- CMake installation and the exported `thrift::c11` target.

Binary and Compact support optional multiplexing over TCP, framed TLS, Unix sockets, HTTP or WebSocket. Zstandard
wraps a byte transport and uses standard independent frames. JSON/Header
protocols and unversioned Binary messages are not implemented. Framing is
currently provided by the TLS server/client adapter. The HTTP client supports HTTPS
through libcurl; the HTTP server can sit behind a TLS reverse proxy. It is not
yet feature-equivalent to `lib/c_glib`.
Application exceptions map to `THRIFT_REMOTE`; their textual details are not
exposed by the current client API. Declared exceptions are preserved in generated
result objects.

See the [TLS guide](tls.md) for the nonblocking API, credentials, platform requirements,
framing and generated-client integration.

See the [WebSocket guide](websocket.md) for the C++ wire model, public APIs,
CMake switches, vcpkg features, TLS usage and the required CivetWeb bounded-reader patch.

The runtime is built as a static library on both platforms. This keeps generated
immutable descriptors valid as link-time C initializers and avoids imported-data
initialization differences across Windows DLL toolchains. The enclosing project's
`BUILD_SHARED_LIBS` option does not change this target.

## Public API reference

All installed runtime headers contain Doxygen documentation for public functions,
parameters, return values, enum values and structure fields. Contracts describe
ownership, borrowed lifetimes, failure behavior and synchronization requirements.
Internal headers are excluded from the API reference.

Generate the HTML reference from the repository root with:

```sh
cd lib/c11
doxygen Doxyfile
```

Open `lib/c11/docs/api/html/index.html` relative to the repository root. The
configuration treats undocumented API members, missing parameter documentation
and malformed Doxygen commands as errors. Doxygen is only needed to generate
documentation, not to build the runtime.

## Logging and constants

`<thrift/c11/thrift_log.h>` provides a central synchronous logging interface.
The default sink writes structured key/value records to stderr at `info` level.
Levels, in increasing severity, are `trace`, `debug`, `info`, `warning`, `error`
and `fatal`. The runtime never terminates the application on a log event.

Socket and server lifecycle operations use `info`; RPC, HTTP, compression and
resource diagnostics use `debug`; individual socket transfers use `trace`.
Instrumented operations emit begin/end records. Nested operations retain their
diagnostic records, while the outer operation reports a propagated failure once
at `error`. A recovered address-attempt failure produces a `warning` describing
the successful fallback. Records include the operation, status, byte bound/count
and native error code when available. Runtime records exclude payloads, URLs,
credentials and remote exception text.

Call `thrift_log_configure_level(cli_level)` during application startup to
select the level: a non-NULL CLI value overrides `THRIFT_LOG_LEVEL`, which
overrides the `info` default. Invalid values return `THRIFT_INVALID` and
leave configuration unchanged. The library does not read the environment
implicitly. The calculator example accepts `--log-level=LEVEL`, for example:

```sh
THRIFT_LOG_LEVEL=info out/c11/examples/thrift_c11_calculator --help --log-level=debug
```

`thrift_log_configure(level, sink, context)` installs an application sink;
NULL selects stderr. Subsequent level configuration preserves the sink. Configure
before starting workers or after joining all runtime users: configuration requires
process-wide exclusive access. The sink must be thread-safe and must not call
runtime/logging functions. Events and their strings are borrowed during the
callback; the configured context must remain alive until logging stops or the
sink is replaced. Platform-specific files provide per-thread operation state.

Sink failures never replace operation return values. The calling worker can
inspect the last delivery result with `thrift_log_last_status()`.
Applications may emit their own sanitized events with `thrift_log_write()`;
guard expensive argument preparation with `thrift_log_enabled()`.

Meaningful sizes, limits, timeouts, protocol flags, CLI positions and test tuning
parameters use named constants. Literal golden wire vectors and representative
test values remain explicit so tests can be checked against the protocol.
Trivial sentinels and immediate bit shifts/masks remain literals, as permitted
by `my_doc/coding_c11.md`.

## Build from the repository root

CMake 3.18 or newer and a C11 compiler are required. Building the Thrift compiler
also uses the existing project's C++ compiler, Flex and Bison requirements.

```sh
cmake -S . -B out/thrift -DBUILD_LIBRARIES=OFF \
  -DBUILD_TESTING=OFF -DBUILD_TUTORIALS=OFF
cmake --build out/thrift --target thrift-compiler --parallel

cmake -S lib/c11 -B out/c11 \
  -DTHRIFT_C11_COMPILER="$PWD/out/thrift/compiler/cpp/bin/thrift" \
  -DTHRIFT_C11_EXAMPLES=ON -DTHRIFT_C11_WERROR=ON
cmake --build out/c11 --parallel
ctest --test-dir out/c11 --output-on-failure
```

The repository-level options are `WITH_C11`, `BUILD_C11`, and
`THRIFT_COMPILER_C11`. The standalone runtime builds without a Thrift compiler;
set `THRIFT_C11_COMPILER` to enable generator tests and examples. It must be a
**host** executable, including during cross compilation.

For ASan, UBSan and leak checking on Linux, add
`-DTHRIFT_C11_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug` to a separate build directory.
The sanitizer flags propagate to generated code, tests and examples.

For native Windows, use MSVC 19.28 or newer, or a C11-capable MinGW toolchain:

```powershell
cmake -S lib/c11 -B out/c11-windows -DTHRIFT_C11_WERROR=ON
cmake --build out/c11-windows --config Debug
ctest --test-dir out/c11-windows -C Debug --output-on-failure
```

To run generated-code and TCP tests on Windows, additionally supply a native
`THRIFT_C11_COMPILER` executable and `-DTHRIFT_C11_EXAMPLES=ON`. Python is optional
and used only by generator-diagnostic and TCP integration tests. Existing vcpkg
CMake toolchains can be used; the C11 runtime has no third-party dependencies.

Windows source and generated bindings have been built with MinGW and validated
natively with MSVC; see the validation section for details. Unix-domain socket
support and its public header/tests are excluded from Windows builds/installations.

## Generate and link bindings

```sh
out/thrift/compiler/cpp/bin/thrift -r --gen c11 your_service.thrift
```

Each IDL file produces `gen-c11/<program>_types.h` and
`gen-c11/<program>_types.c`. That pair contains its types, constants, client calls
and service processors. Compile all generated `.c` files, including those for IDL
includes. The `-out` option selects an existing output directory instead.

Install and consume the runtime with CMake:

```sh
cmake --install out/c11 --prefix out/c11-install
```

```cmake
find_package(thrift_c11 CONFIG REQUIRED)
add_executable(my_service main.c gen-c11/your_service_types.c)
target_include_directories(my_service PRIVATE gen-c11)
target_link_libraries(my_service PRIVATE thrift::c11)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix when configuring the consumer.

In-tree consumers that `add_subdirectory` this repository (like `temp_impl/` and
`examples/`) can use `thrift_c11_add_generated()` instead of wiring
`target_sources`/`target_include_directories`/`target_link_libraries`/warnings by
hand for each generated set:

```cmake
add_executable(my_service main.c)
thrift_c11_add_generated(my_service BASE your_service SERVER_STUBS)
```

It attaches `<base>_types.c` (required), `<base>_client.c` (if present) and, with
`SERVER_STUBS`, the required `<base>_server.c`; `DIR` overrides the search directory
(default `CMAKE_CURRENT_SOURCE_DIR`). It does not invoke the compiler; generate the
files first and regenerate manually after IDL changes. Every call also scans
`<base>_types.h` for its forward-declared struct/enum names and checks them against
every other `thrift_c11_add_generated()` call in the same configure run, failing on
an exact duplicate type name: two independently generated IDLs sharing both a C11
namespace and a type name would otherwise only surface as a link error. This checks
actual declared type names, not just the namespace string, so two IDLs sharing a
namespace with distinct type names (see the namespace note under Generated API and
ownership) link and configure fine. It does not know which targets end up linked
together, so a hit here means "investigate", not "certainly a real conflict" (the
linker remains the final authority) - and it can only see the types actually
forward-declared in `<base>_types.h`, not consts/typedefs/services. Like
`thrift_c11_warnings`, this helper is only available to in-tree consumers, not
through the installed `find_package(thrift_c11)` package.

## Generated API and ownership

Runtime C symbols use `thrift_`, with `THRIFT_` for enum values and macros.
Generated bindings use their namespace directly, without a `thrift_` prefix.
Regenerate bindings and rebuild consumers after this source API change.
No compatibility aliases are provided.
The logging environment variable is `THRIFT_LOG_LEVEL`.
Build/package identifiers remain specific to the C11 implementation:
`--gen c11`, `<thrift/c11/...>`, `find_package(thrift_c11)` and `thrift::c11`.

An explicit `namespace c11 bom` sets the complete generated prefix `bom_`.
The wildcard `namespace *` remains the fallback when no C11 namespace is given.
Without either namespace the prefix is `<program>_`. Names use snake case.
For example, `BomManager` in `CproIntegraBom.thrift` with `namespace c11 bom`
produces `bom_bom_manager_handler` instead of
`cpro_integra_bom_bom_manager_handler`. Choose distinct type/service names
when several IDLs share a namespace. Other language namespaces are unaffected.

IDL enums are real C enums, referenced as `enum <namespace>_<type>`. Their
members use uppercase names such as `ORDER_ORDER_MANAGER_ERROR_CODE_SUCCESS`.
Each enum includes `<NAMESPACE>_<TYPE>_THRIFT_RESERVED_WIDTH = 0x7fffffff` (reserved,
do not use) and a `_Static_assert` requiring the same size as `int32_t`.
The sentinel prevents short-enum compilation from narrowing the representation;
negative values and unknown wire values remain supported. Case-enum members are
also uppercase. IDL typedefs of enums remain typedef aliases of the enum type.

Generated files always use the normalized IDL basename:
`CproIntegraBom.thrift` produces `cpro_integra_bom_types.h` and
`cpro_integra_bom_types.c`, including matching generated include directives.
Header guards use uppercase names such as `CPRO_INTEGRA_BOM_TYPES_H` and remain
per-file even when IDLs share a C11 namespace.
This changes generated source names and symbols: regenerate bindings and update
consumer includes/build rules together; no compatibility aliases are emitted.

IDL documentation comments (`/** ... */`) are emitted as Doxygen comments in
public generated headers. This covers file documentation, typedefs, enums and
values, records (including unions/exceptions), fields, constants, services and
methods. Argument and declared-exception documentation attaches to the generated
args/result fields. Method documentation appears on both client calls and server
callbacks, including inherited methods. Text, paragraphs and Doxygen markup are
preserved; ordinary `//` and `/* ... */` comments are not documentation in the
Thrift parser.

For every IDL containing services, `--gen c11` also produces fully implemented
`<program>_client.h` and `<program>_client.c`. Compile/link the client C file with
the corresponding types C file and the C11 runtime. These files are regenerated
completely; no method bodies need application implementation.

Each service exposes a stack-allocated `struct <service>_client`,
`<service>_client_init(&client, &protocol)` and
`<service>_client_<method>_call(&client, &args, &result)`, including inherited methods.
The client borrows a configured protocol/transport; it does not allocate itself,
open connections, select endpoints, configure credentials or close the transport.
Keep the protocol/transport alive, and serialize calls sharing them. Configure
multiplexing on the protocol before initializing the client when needed.

Sequence IDs start at 1 and wrap from INT32_MAX to 1 without signed overflow.
Invalid arguments do not advance the counter; each attempted RPC does. Clients
return the runtime status unchanged, do not retry, and require connection recovery
after I/O/protocol errors. Oneway calls accept a NULL result and do not wait for a
response. Declared exceptions return THRIFT_OK with the corresponding result case;
application exceptions return THRIFT_REMOTE. Results remain caller-owned and must
be zero-initialized (or valid owned values) before calls and cleared after use.
Existing `_call(protocol, sequence, args, result)` functions remain available for
explicit sequence management.

Client/server RPC validation returns distinct statuses for the first failed check:

| Status | Cause |
| --- | --- |
| `THRIFT_NULL_CLIENT` | Missing client pointer |
| `THRIFT_NULL_PROTOCOL` | Missing protocol pointer |
| `THRIFT_PROTOCOL_UNINITIALIZED` | Protocol has no initialized operations |
| `THRIFT_NULL_ARGS` | Missing RPC argument record |
| `THRIFT_NULL_RESULT` | Missing result for a non-oneway method |
| `THRIFT_INVALID_SEQUENCE` | Automatic client counter is below 1 |
| `THRIFT_NULL_HANDLER` | Missing server handler |
| `THRIFT_NULL_METHOD` | Missing method descriptor or required table |
| `THRIFT_MISSING_CALLBACK` | Handler method is not installed |
| `THRIFT_NOT_IMPLEMENTED` | Generated placeholder callback was reached |

`thrift_status_string(status)` supplies a distinct diagnostic for each code.
Rejected client arguments do not modify results, consume a sequence or perform I/O.
The explicit low-level sequence argument still accepts the complete int32 range.
The server logs callback failures as `rpc.server.handler`, with their exact status
and IDL method name, before replying with a standard Thrift application exception.
Remote callers still receive `THRIFT_REMOTE`; these local codes are not new wire
exception types. A successfully transmitted error reply remains a successful
server dispatch. A NULL application `user_context` remains valid.

Methods with no optional argument and a scalar/enum/UUID/struct return (not
string/list/set/map) additionally get a flattened call when generated with:

```sh
thrift -r --gen c11:flat_calls -out output service.thrift
```

`<service>_client_<method>(&client, <arg>, ..., out_success, out_<exception>, ...)`
takes one C parameter per IDL argument instead of an args record, and one output
pointer per success/declared exception instead of a result record; oneway methods
take no output pointers. All output pointers are required for non-oneway methods
and are written only on `THRIFT_OK`; a NULL output pointer returns `THRIFT_NULL_VALUE`
before any I/O. Struct-typed success/exception values transfer ownership to the
caller. Struct/exception output slots must initially contain NULL; a nonempty slot
returns `THRIFT_OUTPUT_NOT_EMPTY` before I/O or sequence advancement. Output slots
must not overlap; equal addresses return `THRIFT_INVALID_OWNERSHIP`. On
`THRIFT_OK`, inactive outputs are zero/NULL. Clear and free returned owned objects
and reset their slots to NULL before reuse. Failure leaves all outputs unchanged.
An empty result is valid for a void method, including methods declaring exceptions;
only a nonvoid method with neither success nor an exception returns
`THRIFT_MISSING_RESULT`. Arguments are borrowed for the duration of the call.
Generated input parameter names use `arg_<position>_<field>` to avoid C keywords
and collisions with local/output names; byte arguments add `_data` and `_size`.
Methods with an optional argument or a string/list/set/map
success type keep using `_client_<method>_call()` with the args/result records;
`flat_calls` is purely additive and never removes that function.

Optional server implementation skeletons are generated with:

```sh
thrift -r --gen c11:server_stubs -out output service.thrift
```

Each IDL containing services gets `<program>_server.h` and `<program>_server.c`.
For each service, `_server_init(handler, user_context)` installs all callbacks, including
inherited methods, and borrows the application context. `_server_process(protocol,
handler_context)` is a correctly typed adapter for the server runtime's `(protocol, void *)`
callback. `handler_context` points to the generated handler object; the dispatcher
passes `handler->user_context` as the separate `user_context` argument of a method
callback. `_server_init()` stores the caller's application pointer in that field.
The existing `_service(handler)` registration supports multiplexing.
The skeleton is transport-independent and has no `main()` or socket policy.

Edit the static `_server_handle_<method>()` callbacks to implement application logic.
Use result setters and return `THRIFT_OK` for success or a declared exception.
Arguments are borrowed; copy them before placing data in an owned result.
Unimplemented callbacks return `THRIFT_NOT_IMPLEMENTED`, producing an application-error
reply for ordinary calls; oneway calls cannot receive error replies.

With both `server_stubs` and `flat_calls`, eligible methods (the same rule as the
flat client call) instead get `<service>_server_<method>(user_context, <arg>, ...,
out_success, out_<exception>, ...)` to implement, matching the flat client call's
parameter shape. It is declared in the regenerated `*_server.h` and defined in the
preserved `*_server.c`, alongside a generated `static inline` adapter (also in
`*_server.h`) that plugs it into the fixed handler slot. On `THRIFT_OK`, populate
exactly one output (or none, for a void result with no exception); the outputs
alias the dispatcher's own zeroed result storage, so no pre-NULL or aliasing checks
are needed on the callback side. Copy borrowed argument data before storing it in
an owned output. Ineligible methods keep the record-based `_server_handle_<method>()`.

Types and server headers are regenerated. Existing `*_server.c` files are preserved
byte-for-byte, so implementation edits survive regeneration. Existing implementations
using the former handler field `context` must rename it to `user_context`; method
callbacks use `user_context`, while runtime adapters use `handler_context`. After IDL changes,
generate into a fresh directory and merge changed callbacks/initializers manually.
Do not run concurrent generators against the same output directory. Link the server
source with its generated types and `thrift::c11`; no extra dependencies are needed.

`--gen c11:manifest` additionally writes `<program>_manifest.json`, a flat JSON array
of `{"idl": "<dotted IDL name>", "kind": "...", ...}` objects covering typedefs,
enums (and their values), consts (with `getter`/`clear`), structs/unions/exceptions
(and their fields, with `member`/`setter`), services and methods (with `call`,
`client_call`, `args_record`, `result_record`, and `flat_client_call`/`server_callback`
when `flat_calls`/`server_stubs` are also enabled). It exists so a tool, script or an
LLM without the generated header in context does not have to reimplement the
acronym-aware snake_case/prefix naming rules to guess a symbol name; look it up by
its IDL name instead. `kind: "method"` and `kind: "const"` use `stem` rather than
`c_name`: `<owner>_<method>` and `<program>_const_<name>` are not themselves callable
symbols, only naming prefixes for the accessors alongside them (`getter`/`clear` for
consts; `call`/`client_call`/etc. for methods). A method's `idl` key is
`<service actually generating this entry>.<method>`, not `<declaring service>.<method>`:
an inherited method gets one entry per subclass that exposes it, each with that
subclass's own `client_call`/`flat_client_call`/`server_callback`, so keying by the
declaring service alone would make two entries share one ambiguous key; `owner` still
names where the method was declared. Combine freely with the other options, e.g.
`--gen c11:server_stubs,flat_calls,manifest`.

Generated records remain transparent and can be stack-allocated with `{0}`.
Prefer `_set_<field>()` over assigning both `f_<field>` and `has_<field>`:

- Scalar/enum/UUID setters take a value; ordinary-record setters are `static inline`.
- String/binary setters take `(record, data, size)` and copy through `thrift_bytes_set()`.
  Failure leaves both the value and presence flag unchanged; overlapping input is safe.
- Record-pointer setters take ownership of a non-NULL allocated pointer, releasing
  the old value. Assigning the same pointer again is safe. Descendant aliases and
  shared ownership are forbidden.
- List/set/map setters take a pointer to an owned container, move it into the field,
  zero the source, and release the previous destination. Self-move is safe.

All setters return `enum thrift_status` and require a zero-initialized or valid
owned destination. They set the presence flag only on success. `_new()` allocates
and applies defaults; it returns NULL on failure. Release its
result with `_clear()` followed by `free()`. Stack storage still uses `_init()`
and `_clear()` directly. A failed setter does not take ownership of its input.
For explicit allocation errors, use `<record>_create(&pointer, budget)`.
The pointer must initially be NULL; failure preserves it. The optional cumulative
budget covers the record and its defaults; consumed budget is not refunded.
`_new()` delegates to `_create()` without a budget.

Generated record/constant helpers distinguish NULL destinations
(`THRIFT_NULL_VALUE`), NULL field pointers (`THRIFT_NULL_FIELD`), missing nonempty
storage (`THRIFT_NULL_DATA`, `THRIFT_NULL_MAP_KEYS`, `THRIFT_NULL_MAP_VALUES`),
inconsistent list capacity (`THRIFT_INVALID_CAPACITY`), direct self-ownership or
shallow aliases of destination storage (`THRIFT_INVALID_OWNERSHIP`), and nonempty
allocation outputs (`THRIFT_OUTPUT_NOT_EMPTY`). Size/budget failures return
`THRIFT_LIMIT`; allocator failures return `THRIFT_NOMEM`.
These checks do not validate arbitrary pointer provenance or descendant aliases.

`thrift_bytes_set_cstr(&bytes, "text")` copies a NUL-terminated string and rejects NULL.

Exclusive records (unions and RPC results, including declared exception alternatives)
also expose `enum <record>_case` and `<record>_case()`. Cases are `<RECORD>_CASE_NONE`,
`<RECORD>_CASE_FIELD_<FIELD>`, and `<RECORD>_CASE_INVALID` for NULL or conflicting presence flags.
Each successful setter clears all other alternatives without restoring defaults.
Byte setters allocate their replacement before clearing the old alternative, so
allocation/validation failure preserves the selected value. Standalone exception
records are ordinary records and may have several fields. Direct field access
remains possible; using setters maintains exclusivity, while manual edits remain
subject to accessor/serialization validation.

`thrift_list_reserve(&list, count, element_size, budget)` reserves capacity without
changing `size`. `thrift_list_append(&list, &element, element_size, budget)` appends a
native slot. Use `descriptor->element->size` (or the matching C `sizeof`) for the
slot size; struct slots are pointers, so pass the address of the pointer.
These helpers serve both lists and sets; set uniqueness remains caller-owned.
Append transfers ownership of descendants by copying their slot, so reset the
source after success without freeing it. Scalar self-append is supported; copying
an existing owned element into another slot would create forbidden shared ownership.
A NULL budget means unlimited allocation. Growth charges the complete new buffer;
failed operations preserve the list and budget. Reserve once when a count is known
for predictable allocation; append otherwise grows geometrically, falling back to
exact growth when the remaining budget is tight.

`struct thrift_list` now includes `capacity`: regenerate bindings and rebuild all
consumers together (ABI change). Protocol reads and generated defaults/constants
populate it. Manually allocated lists with zero capacity are accepted as having
`size` allocated slots. Clearing a list zeroes its size, data and capacity.

`thrift_map_reserve(&map, count, key_size, value_size, budget)` and
`thrift_map_append(&map, &key, &value, key_size, value_size, budget)` are the map
equivalent, growing the parallel `keys`/`values` arrays together; `struct thrift_map`
also gained `capacity` for the same reason and with the same ABI/regeneration note.
This never looks up or replaces an existing key: key uniqueness remains caller-owned,
matching list/set semantics. Growth charges keys and values together against one
budget, bounded by their combined per-pair cost, not each array checked separately.

Fields are `f_<name>` and presence flags are `has_<name>`.
The prefixes avoid C keywords. Ambiguous names after normalization are rejected.
Constants use a separate `const_<name>` component.

- Start values with `{0}`, or call the generated `_init()` on an empty object to
  apply defaults. `_init()` can allocate and returns a status. Clear an existing
  object before initializing it again.
- `_clear()` recursively releases owned storage and resets the value. It is safe
  on a zeroed or already cleared object. Struct-valued fields are owned pointers;
  recursive object graphs must be acyclic and must not share owned allocations.
- Strings and binary are `struct thrift_bytes`. `size` is authoritative,
  embedded NUL bytes are supported, and `_bytes_set()` copies the input. Nonempty
  copied strings and decoded strings include a trailing NUL for convenience.
- Lists and sets are `struct thrift_list`: `size` elements in a `data` array
  of their native element type. Struct elements are pointers. Maps store parallel
  `keys` and `values` arrays. The caller maintains set/map uniqueness.
- Assign owned pointers allocated with `malloc`/`calloc`. Do not install pointers
  to stack storage or literals in objects that will be cleared.
- Enum storage is `int32_t`; unknown numeric enum values are retained.
- Optional fields are written only when `has_<name>` is true. Required and
  default-requiredness fields are written regardless of that flag. Reading checks
  that every explicitly required field appeared with the correct wire type.
  Explicit defaults initialize values and mark them present, matching C++.
  A missing optional field retains that initialized default and presence flag.
- Unions and method results allow at most one present member. Reading a present
  union field replaces a default selection. Empty unions are allowed.
- `_const_<name>_get()` replaces an initialized destination with an owned copy;
  `_const_<name>_clear()` releases it. Typedefs also provide `_clear()` wrappers.
- Successful `_read()` replaces an initialized destination. On failure it leaves
  the destination unchanged and releases partially decoded storage.
- Descriptors are trusted, immutable compiler output. Applications should not
  construct inconsistent descriptors or alter sizes after allocating containers.

Generated `_call(protocol, sequence, args, result)` functions are synchronous.
The caller supplies sequence IDs and initializes the result. Successful methods
set `has_success`; declared exceptions set their corresponding result flag and
still return `THRIFT_OK`. Nonvoid methods without a success/exception produce
`THRIFT_MISSING_RESULT` at the client. Oneway calls permit a NULL result.

A generated handler contains a borrowed `context` and typed callbacks. Callback
arguments are borrowed for the duration of the call. Result storage is owned by
the processor: populate owned fields and let the processor release them after
writing the response. Returning a non-OK status creates a generic application
exception for ordinary calls. A missing callback does the same. Oneway callback
errors propagate locally without emitting a reply.

`_process()` handles one message on an existing protocol. The simple server
helper accepts one connection, processes a caller-selected number of requests,
and closes it. Applications control their accept loop, shutdown and concurrency.
Independent objects are reentrant; concurrent access to the same mutable object,
protocol, socket, memory transport or handler requires external synchronization.

## Binary, Compact and multiplexed services

The generated bindings work with either serialization protocol without
regeneration. `thrift_protocol_init()` and
`thrift_binary_protocol_init()` select versioned Binary.
`thrift_compact_protocol_init()` selects Compact v1. The common
`thrift_protocol_init_kind()` accepts `THRIFT_BINARY` or
`THRIFT_COMPACT` explicitly. The simple server's
`thrift_server_serve_kind()` selects the same protocol for accepted clients.
Peers must agree on the protocol; there is no automatic sniffing.

Compact implements bounded varints, unsigned ZigZag conversion for signed
integers, delta-encoded field IDs, bool values embedded in field headers,
little-endian doubles and compact container headers. Field-ID state is local to
each recursive struct. Overflowing/unterminated varints, invalid type nibbles and
field-ID overflow are rejected through the normal status model.

Multiplexing prefixes request names as `service:method`; replies keep the bare
method name. For a Compact client, initialize the protocol with:

```c
status = thrift_multiplexed_protocol_init(
    &protocol, transport, THRIFT_COMPACT, "Calculator");
```

Include `protocol/thrift_multiplexed_protocol.h` under the `thrift/c11` include
prefix. The service-name string is borrowed for the protocol's lifetime. Service
names must be nonempty and contain no colon.

On the server, generated `_service()` functions expose borrowed method tables:

```c
struct thrift_service services[] = {
    example_calculator_service(&handler)
};
status = thrift_multiplexed_process(&protocol, services, 1, NULL);
```

Include `processor/thrift_multiplexed_processor.h`. Use one table entry per
service, optionally replacing an entry's `name` with a borrowed alias. Duplicate
or malformed service names are rejected before reading a message. Pass a
registered service name as the final argument to accept unprefixed requests for
that default service; NULL requires explicit multiplexed names. Unknown services
produce application exceptions for ordinary calls and no reply for oneway calls.
The same processor works with Binary and Compact protocol objects. Registrations
and handlers must remain alive for the duration of dispatch.

## Unix-domain sockets (Linux)

Include `<thrift/c11/transport/thrift_unix_socket.h>` and open a stream socket with
`thrift_socket_connect_unix(path, &client)` or
`thrift_socket_listen_unix(path, &listener)`. Both return an owned
`struct thrift_socket *` on success and clear the output pointer on failure.
Check the returned `enum thrift_status` before using the handle.

Use the existing `thrift_socket_accept()`, `thrift_socket_timeout()`,
`thrift_socket_transport()` and `thrift_socket_close()` functions. The synchronous
`thrift_server_serve_kind()` helper accepts Unix listeners directly. Generated
clients/processors work unchanged with Binary, Compact and multiplexing. A Zstd
wrapper can use the resulting byte transport. The HTTP adapters are separate.
The calculator CLI remains a TCP example; `tests/test_unix_rpc_posix.c` shows
complete generated client/server use over Unix sockets.

Paths refer to filesystem sockets, not Linux abstract addresses. Relative paths
use the current working directory; paths must fit the platform's `sun_path`
including the terminating NUL. Overlong paths return `THRIFT_LIMIT` without
truncation. Unix sockets have no TCP port: `thrift_socket_port()` returns
`THRIFT_INVALID` and leaves its output unchanged.

The application owns the pathname and its parent directory. Binding never
removes existing files, symlinks or sockets. Permissions follow the process umask.
Closing releases the socket handle but leaves the pathname; remove the
application-owned socket with `unlink()` after closing the listener, before a
future bind. A path can also remain if bind succeeds but listen subsequently
fails. The runtime performs no automatic stale-socket deletion. Accepted/client
sockets do not own or remove the listener path.

The implementation stays in `thrift_socket_posix.c`; the Unix-specific header is
only installed on POSIX. There are no Windows implementations or fallback stubs.
Run the Linux socket and independent Python interoperability tests with
`ctest --test-dir <build> -R thrift_c11_unix_ --output-on-failure`.

## Limits and error handling

Defaults are 16 MiB of wire data, 32 MiB of cumulative allocation, one million
container entries, and nesting depth 64 per message. Adjust `protocol.limits`
and call `thrift_protocol_reset()` before standalone serialization. RPC calls
reset budgets automatically at message boundaries. Allocation accounting includes
decoded values, field-presence tracking and generated defaults. Container counts,
allocation products and signed wire lengths are checked before allocation.

Every fallible API returns `enum thrift_status`. Instrumented runtime operations
report failures through the central logger with operation context. Close a connection
after an unsuccessful protocol operation: the stream may no longer be aligned.
Read/write timeouts bound individual blocking socket operations, not the entire
RPC. DNS resolution, connect and accept follow the operating system's blocking
behavior. A NULL listening hostname binds loopback; use an explicit bind address
to expose a server beyond localhost.

## Example and tests

Run in separate terminals after building with `THRIFT_C11_EXAMPLES=ON`:

```sh
out/c11/examples/thrift_c11_calculator --server 9090
out/c11/examples/thrift_c11_calculator --client 9090
```

To use Compact plus multiplexing, run:

```sh
out/c11/examples/thrift_c11_calculator --server 9090 compact Calculator
out/c11/examples/thrift_c11_calculator --client 9090 compact Calculator
```

The client prints `Result: 42`. The example server serves one request and exits;
`--server 0` selects a free port. `--help` requires no network access.

Tests cover Binary/Compact wire bytes, signed extrema, all scalar/container types,
recursive ownership, defaults, unions, includes, inheritance, exceptions, oneway
calls, truncated/mutated inputs, allocation/recursion/count limits, sequence
mismatches, malformed Compact varints/field IDs, multiplexed routing, invalid
generator input and real TCP communication. Codec tests compare complete nested
structures byte-for-byte with the existing Python runtime. TCP tests validate
C11 clients and servers against Python peers for both protocols with and without
multiplexing, including declared/application exceptions and unknown fields.

The supplied coding guide references `cpro_unity_test()` and
`cpro_smoke_test()`, which are absent from this Thrift source tree. Tests use
CTest and release-active C checks instead of introducing that unrelated framework.
The provided workspace also has an empty read-only `.git` directory, so the
prescribed branch/commit workflow could not be performed in this environment.


## Optional Zstandard and HTTP transports

Enable only the dependencies needed by the application (all default to `OFF`):

```sh
cmake -S lib/c11 -B out/c11 \
  -DTHRIFT_C11_ZSTD=ON \
  -DTHRIFT_C11_HTTP_CLIENT=ON \
  -DTHRIFT_C11_HTTP_SERVER=ON
cmake --build out/c11 --parallel
```

Dependencies: Zstandard >= 1.4, libcurl >= 7.85 and CivetWeb (tested with 1.16).
They are discovered through CMake packages; consumers of an installed
`thrift::c11` target inherit the link dependencies. The installed package exports
`thrift_c11_ZSTD`, `thrift_c11_HTTP_CLIENT` and `thrift_c11_HTTP_SERVER` booleans.
On Windows use the existing vcpkg toolchain and a consistent triplet, for example:

```powershell
C:/work/vcpkg/vcpkg.exe install zstd:x64-windows curl:x64-windows civetweb:x64-windows
cmake -S lib/c11 -B out/c11-windows -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/work/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DTHRIFT_C11_ZSTD=ON -DTHRIFT_C11_HTTP_CLIENT=ON -DTHRIFT_C11_HTTP_SERVER=ON
cmake --build out/c11-windows --config Release
ctest --test-dir out/c11-windows -C Release --output-on-failure
```

Windows availability was checked against the official vcpkg ports for
[zstd](https://vcpkg.io/en/package/zstd.html),
[curl](https://vcpkg.io/en/package/curl.html) and
[civetweb](https://vcpkg.io/en/package/civetweb.html). No additional platform
branches are needed: these libraries provide the platform implementations.

### Zstandard

`transport/thrift_zstd_transport.h` provides `thrift_zstd_create()` and
`thrift_zstd_destroy()`. Creation takes a borrowed underlying transport, a
positive maximum uncompressed frame size, a compression level (0 selects the
Zstandard default), and output pointers for the owned handle and transport view.
Pass that view to the usual Binary/Compact protocol initializer.

Writes accumulate until `flush()`, which emits one complete standard Zstandard
frame with a content checksum and then flushes the underlying transport. Multiple
flushes produce concatenated frames. Reads collect exactly one frame at a time,
validate/decompress it, and expose its bytes. This avoids reading into a subsequent
RPC on blocking sockets. It is standard Zstandard framing, not Thrift's four-byte
length-prefixed framed transport and not compatible with the C++ Zlib wire format.
The checked-in C++ library contains `TZlibTransport`, but no Zstandard transport.

The compressed-input bound is `ZSTD_compressBound(max_frame_size)`; uncompressed
input/output each have the configured bound. Dictionaries and skippable frames
are rejected. Reads accept up to 64 empty frames per call before returning
`THRIFT_LIMIT`. Destroy releases buffers and never flushes or closes the
borrowed transport. Any failure poisons the wrapper; discard the connection.
Zstandard does not negotiate compression or HTTP Content-Encoding: both peers
must explicitly select matching transport stacks.

### HTTP client

`transport/thrift_http_client.h` provides explicit process-wide library init and
cleanup, plus client create/destroy. Call library init before starting threads,
then create a handle with `thrift_http_client_options`: URL, positive body
limit, positive timeout in milliseconds (at most `INT_MAX`), and optional CA file.
URLs and CA file paths are copied by libcurl. Use the returned transport with
normal generated client calls; the generated RPC code calls `flush()`.

Use `thrift_http_client_set_bearer_token(client, token)` before the next request
to enable or replace Bearer authentication; pass `NULL` to remove it. libcurl
copies the token. The API accepts RFC 6750 token characters and trailing `=`
padding, up to `THRIFT_HTTP_BEARER_TOKEN_MAX` bytes. Empty tokens, header injection
and excessive lengths are rejected without changing the previous token. Tokens
are never logged. Use HTTPS for remote credentials. Login and automatic refresh
remain application responsibilities. The setter requires exclusive client access.

Flush sends a POST with `Content-Type` and `Accept: application/x-thrift` and
buffers the complete response. Only HTTP 200 is accepted; other statuses return
`THRIFT_REMOTE`. Redirects are disabled, TLS certificate/hostname verification
remains enabled, and libcurl handles HTTP framing, chunked responses and proxies.
The body limit applies independently to request and response. Timeout bounds
connect and transfer time; DNS timeout behavior also depends on libcurl's resolver
backend. Handles require exclusive access. Destroy all handles before library
cleanup. Applications already managing libcurl globally may use its lifecycle API.

### HTTP server

`server/thrift_http_server.h` provides library init/cleanup and server start,
port query and stop. Start takes options, a processor callback and a borrowed
handler. Options include a CivetWeb listen address such as `127.0.0.1:9090`, an
exact path such as `/rpc`, positive body limit and timeout, Binary/Compact kind,
and optional protocol limits. Port zero chooses an available port. The address
accepts one HTTP listener, not CivetWeb SSL/redirect suffixes or listener lists.

One CivetWeb worker invokes the processor once per POST. A multiplexed processor
can dispatch to several generated services. Handler state must remain alive until
stop returns; synchronize accesses from other application threads. Stop waits for
worker shutdown and must not be called from the processor callback. Call library
init before starting threads and cleanup after stopping all servers. The CivetWeb
build must expose at least one non-TLS feature flag so init failures can be detected.

`transport/thrift_http_server_transport.h` also exposes the request transport for
applications with an existing CivetWeb server. Create it inside a request handler,
process the RPC, call finish with the processor status, then destroy it. The
connection stays borrowed. Output remains buffered until finish; malformed or
trailing request bytes cannot commit a partial RPC response. Processing may already
have invoked the application handler before a trailing-byte error is detected.

Both Content-Length and chunked request bodies are bounded. Successful oneway calls
send HTTP 200 with an empty body. Other paths and methods are rejected; no static
files are served. The built-in server is HTTP only. It follows the C++
`THttpClient`/`THttpServer` POST/body semantics, without their permissive CORS policy.

Optional tests cover independent libzstd encoding/decoding, checksums, every
truncation of a frame, size limits, repeated HTTP RPCs, Oneway/application errors,
Binary/Compact/multiplexing against Python Thrift, chunked HTTP in both directions,
HTTP status/redirect handling, body limits and client timeouts.

### Validation

All 65 Linux tests (including Cadin, WebSockets and the optional C++ WebSocket peer) pass with GCC 14, AddressSanitizer and
UndefinedBehaviorSanitizer, including Unix socket lifecycle, path boundaries,
timeout/EOF and Binary/Compact/multiplexed C/Python peers in both directions.
The 62 Windows tests, including Cadin and WebSockets, also passed natively with MSVC 19.44, Release,
`/W4 /WX` and vcpkg `x64-windows-static`. The native test compiler was built from
this checkout's compiler sources and C11 backend, using already generated
Flex/Bison parser sources. Windows tests use that native compiler to generate the
fixtures and calculator. A MinGW crossbuild with all three dependencies also
builds the library, tests and example successfully. Logging tests cover filtering,
CLI/environment precedence, nested failure reporting, recovery diagnostics,
thread-local state and sink failure isolation.

For `x64-windows-static`, select the matching MSVC runtime when configuring:

```powershell
'-DVCPKG_TARGET_TRIPLET=x64-windows-static' `
'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
```

Set `THRIFT_C11_COMPILER` to a native compiler with `--gen c11`, and enable
`THRIFT_C11_EXAMPLES`, to include the generated RPC and TCP/HTTP interoperability
tests. Python interoperability tests additionally require Python and this
checkout's `lib/py/src` directory. Core-only builds remain dependency-free.

The [C++ test adaptation map](tests/cpp_test_adaptations.md) identifies the original
scenarios, C11 equivalents, intentional API differences and excluded features.

The [IDL conformance suite](tests/conformance/README.md) adds semantic checks for
Binary/Compact, compile/link checks for 14 original Thrift IDLs, 27 invalid-input
cases and a separately labelled known frontend limitation. Run it with
`ctest --test-dir <build> -L c11_conformance --output-on-failure` after building.
Its 28 CTest entries provide regression coverage, not a claim of complete language
conformance. In particular, annotations have no C11 semantic mapping, and this
checkout's shared frontend rejects a named struct constant as a field default.

The optional [Cadin integration](examples/cadin/README.md) adds generation and
compile/link coverage for eight unchanged external IDLs and a read-only
`StatusSvc.getApiVersion` client using Compact/Multiplexed HTTP with Bearer
authentication. Set `THRIFT_C11_CADIN_ROOT` to its `apisrv.parent` checkout to
enable it. Generated record type descriptors now use the suffix
`_type_descriptor`; regenerate existing IDLs and update direct descriptor
references when upgrading from `_type`. This resolves the Cadin enum/descriptor
name collision without changing its IDLs or the Thrift wire format.


Generator validation rejects public identifier/guard collisions across the include
graph, duplicate record field IDs (including the implicit RPC success field with
ID 0), and normalized program/namespace prefixes that do not start with a letter.
It buffers each program's output until validation succeeds. Recursive generation
is not a transaction across all output files: an earlier included program may
already have been written when a later program fails.
