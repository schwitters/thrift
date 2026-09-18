# C11 IDL conformance regression suite

The reference language is the Apache Thrift grammar in this checkout,
`compiler/cpp/src/thrift/thrifty.yy`, with lexical rules in `thriftl.ll`.
This is a growing regression suite, not a proof that every valid IDL program
works or that C11 implements other generators' annotations.

## Running

Configure the C11 CMake project with `BUILD_TESTING=ON` and a host
`THRIFT_C11_COMPILER` supporting `--gen c11`. In the repository build, the native
`thrift-compiler` target can supply that compiler instead. Build before running:

```sh
cmake --build out/c11 --parallel
ctest --test-dir out/c11 -L c11_conformance --output-on-failure
```

For Visual Studio, add `--config Release` to the build command and `-C Release`
to CTest. Generated C is compiled by the target toolchain, including when cross
compiling; executing the resulting binaries requires the target platform or an
emulator. The C++ compiler must run on the build host.

The suite needs the repository's `test/*.thrift` corpus. Missing corpus fixtures
are configuration errors rather than silently skipped coverage. Python enables
the rejection and known-limitation groups; neither optional transports nor network
access are needed for these tests. `THRIFT_C11_WERROR=ON` also checks generated
sources with warnings treated as errors. Sanitizers propagate from the runtime.

## Coverage and evidence

There are 28 additional CTest entries: 12 semantic cases, 14 corpus compile/link
smokes, one group of 27 invalid-input cases, and one known-limitation probe.
Labels distinguish these kinds of evidence:

| Label | What a passing test establishes |
| --- | --- |
| `semantic` | Six scenarios each run with Binary and Compact. Generated constants/defaults are checked against explicit expected values; records or RPCs traverse the selected protocol. |
| `compile_link` | Original repository IDL generates warning-clean C11, its header compiles as the first include, and all generated objects link into a runnable executable. These are not semantic tests of every object in the corpus. |
| `negative` | Every invalid input exits with the expected rejection status, emits a relevant diagnostic, and leaves no generated C/header in its fresh output directory. Crashes/timeouts do not count as rejection. |
| `known_limitation` | A documented unsupported valid-language intention still fails as expected. A pass records the limitation; it does not claim language support. Unexpected acceptance requires promoting the case to a positive regression. |

| Language area | Concrete checks |
| --- | --- |
| Headers/names | Includes, imported types/constants/services, wildcard namespace fallback, explicit `c11` namespace precedence, C keywords and permitted Thrift keywords as field names. |
| Scalars/constants | Boolean values, byte/i8 aliases, signed integer extrema, hex and negative hex, exponent and integer double literals, escaped strings, binary, mixed-case/braced UUID, enum auto-numbering after a negative value, local/imported constant references. |
| Typedefs/containers | Chained scalar aliases, imported struct aliases, enum-keyed maps of lists of struct aliases, set constants, empty list/map/set. Other base types and nested containers are also exercised by the existing generated and C++-adapted protocol tests. |
| Records/defaults | Required/optional/default requiredness, present explicit defaults matching C++, defaults transmitted to a peer with different defaults, independent owned copies, replacement of decoded objects, absent required field despite a default, implicit negative field IDs, maximum positive field ID, empty records. |
| Recursion/unions | Mutually recursive forward struct references, recursive lists, finite owned object trees, default union selection, replacement by another member, rejection of multiple selected members. |
| Services | Two-level inheritance across an include, inherited synchronous call, void return with declared exception, oneway call with no response, empty service, descriptor checks for a struct return with two declared exceptions. Existing RPC tests cover successful struct return, multiplexing and application errors. |
| Grammar metadata | Namespace/type/typedef/enum/field/record/method/service annotations, `cpp_include`, current/deprecated `cpp_type`, XSD metadata, reference marker `&`, deprecated `async` spelling. These parse and compile; they do not activate foreign-generator behavior. |
| Invalid input | Syntax, undefined/duplicate types, duplicate field IDs/names, integer/enum range errors, nested/default range errors, wrong constant types, unknown constant fields, invalid UUID, missing base service, non-exception throws, oneway return/throws, multiple union defaults, normalized type/field/method/inherited-method collisions. |

The 14 unchanged corpus files are `ThriftTest`, `Recursive`, `ManyTypedefs`,
`ManyOptionals`, `EnumTest`, `EnumContainersTest`, `ConstantsDemo`,
`DoubleConstantsTest`, `OptionalRequiredTest`, `TypedefTest`,
`VoidMethExceptionsTest`, `ExceptionStruct`, `DebugProtoTest` and `SmallTest`.
Their expected warnings about legacy syntax, implicit IDs, large constants or
foreign namespaces are distinct from C compiler warnings.

`metadata.thrift` also covers the `ExtendedAttribute` / `ExtendedAttributeType`
record/enum pair that exposed a generated descriptor name collision in Cadin.
The new `_type_descriptor` suffix is checked through Binary/Compact round trips,
a struct alias and a recursive container. Setting `THRIFT_C11_CADIN_ROOT` adds
one corpus test that generates, compiles and links all eight original Cadin IDLs;
see the [integration guide](../../examples/cadin/README.md).

Test payload values are intentionally literal reference data. Buffer capacities,
CLI positions, descriptor positions/counts and process timeouts have named
constants. C assertions remain active in Release and converge on cleanup on
failure; Python cases use separate temporary directories and bounded subprocesses.

## Findings and boundaries

The suite exposed acceptance of `oneway i32 method()`: the shared parser only
warned, while the C11 runtime cannot deliver a result for a oneway call. The C11
generator now rejects a non-void oneway method before writing its output.

This checkout's shared frontend rejects a named struct constant as a field
default, for example:

```thrift
struct Item { 1: i32 value }
const Item VALUE = {"value": 7}
struct Holder { 1: optional Item item = VALUE }
```

The same failure occurs with `--gen cpp`; it is not introduced by the C11 backend.
The known-limitation test preserves this observation. An inline struct literal
default is supported and tested. This iteration does not change the shared parser.

Annotations and foreign-language/XSD metadata are accepted without a C11 semantic
mapping. Reference markers do not change C11's owned-pointer model. Set/map
uniqueness remains the caller's responsibility. Name normalization collisions
are deliberate C11 rejections. The suite does not exhaust every grammar
combination, every annotation, lexer escape, include graph, malformed input or
allocation-failure path; it is not a fuzzing campaign. Existing wire compatibility,
transport and hostile-message tests remain separate and complementary.
