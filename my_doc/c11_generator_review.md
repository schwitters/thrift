# C11 generator review — 2026-09-17

## Scope

Reviewed all emission paths in `t_c11_generator.cc`: naming and include graphs,
typedefs and enums, constants and defaults, record descriptors, initialization and
cleanup, field setters and exclusive cases, inherited services, low-level RPC
wrappers, stateful clients, and editable server skeletons. This is a review of
the C11 backend and the runtime interfaces it calls, not all Thrift language
backends.

## Findings and changes

- Numeric program names produced invalid C identifiers/header guards. Normalized
  program and namespace prefixes must now start with a letter.
- Enum members could collide with generated header guards. Guards participate in
  symbol validation, including client and optional server headers.
- Separate included IDLs sharing a namespace could emit duplicate public symbols.
  A validation-only pass checks public symbols throughout the include graph.
  Distinct declarations may still share a namespace; private container descriptors
  are not treated as public symbols.
- IDL documentation containing a literal opening comment marker produced nested
  C comments and failed strict builds. Both comment delimiters are now escaped
  when rendering Doxygen.
- A declared exception with field ID zero could collide with the synthesized
  success field of a nonvoid RPC result. Duplicate record field IDs now cause a
  generator error.
- Record helpers, setters and constant accessors used an undifferentiated invalid
  status for NULL values. They now identify missing destination, field or storage.
  Record read/write wrappers also distinguish missing/uninitialized protocols.
- Owned pointer setters accepted direct self-ownership. Container setters accepted
  missing arrays, invalid capacity and shallow copies of their own destination
  storage. These cases now fail before modifying ownership or presence flags.
  Separate statuses identify invalid ownership, capacity, map keys and map values.
- Added `<record>_create(&pointer, budget)` for an explicit allocation status.
  It checks the output pointer, charges allocation/default initialization to the
  budget, cleans partial defaults on failure and publishes only complete objects.
  Existing `_new()` delegates to it and retains its NULL-on-failure contract.
- The conformance compile corpus previously omitted generated client source files.
  Service-bearing fixtures now compile and link them too, including an empty
  service.

Client and server checks retain distinct NULL-client/protocol/args/result/handler,
invalid-sequence, missing-callback and unimplemented-handler statuses. Invalid
client arguments do not advance the sequence or perform I/O. Declared exceptions
remain result alternatives; application exceptions retain standard wire behavior.

## Validation

Regression tests cover invalid names and guard collisions, cross-include collisions
and valid shared namespaces, duplicate success/exception IDs, nested comment
markers, allocation budget failures (including default initialization), nonempty outputs,
invalid container storage/capacity, ownership aliases and preserved union cases.

- Linux: compiler, runtime and generated sources build successfully; 57/57 tests.
- ASan/UBSan: 53/53 tests, including generated API and conformance tests.
- Eight Cadin IDLs in `temp_impl` regenerated; types, clients and server skeletons
  compile successfully on Linux.
- Windows/MSVC: compiler, runtime and Cadin bindings build successfully; 62/62
  tests, including HTTP/WebSocket interoperability.

The suites include Binary/Compact round trips, inherited and oneway RPCs,
multiplexing, unknown enum values, signed limits, constants/defaults, malformed
input, logging, generator rejection tests and transport interoperability.
Compile-only corpus tests establish compilation/linkage, not semantic correctness
of every possible schema.

## Contracts and remaining limits

Transparent records still require initialized, valid owned storage. The API cannot
prove pointer provenance or detect arbitrary descendant aliases, overlapping
interior pointers, or cyclic ownership graphs. Owned graphs must remain acyclic and
unshared. Set/map uniqueness remains the caller's responsibility.

`_clear(NULL)` remains a documented no-op; `_case(NULL)` returns the invalid case.
`_new()` deliberately reports failure as NULL; use `_create()` for a precise status.
Budgets are cumulative and are not refunded after failed initialization.

Validation is buffered per program, not transactional across recursive generation.
Earlier included outputs can exist when a later program fails. Existing editable
server C files are preserved; generated types/client files and headers are refreshed.
Concurrent generators must not target the same output directory.

No claim of formal verification or complete Thrift-language conformance is made.
Existing frontend/metadata limitations remain documented in the conformance suite.
No new dependencies or platform conditionals were introduced.

The workspace has no usable Git repository; branch/commit creation was unavailable.
