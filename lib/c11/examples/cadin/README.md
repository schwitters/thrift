# Cadin IDL integration and read-only client

This optional integration generates C11 types, synchronous clients, handler
interfaces and processors from the eight original IDLs in a Cadin
`de.cerpsw.cadin.apisrv.parent` checkout. It reads that checkout without modifying
or copying its IDLs into this repository. The generated objects cover twelve
services across Common, Bom, Mdmp, MdmpBulk, Nodes, NodesIndex, Order and RawRecord.

## Build

Build a host Thrift compiler from this checkout first. The compiler must include
the updated `c11` generator. From the Thrift repository root:

```sh
cmake -S lib/c11 -B out/cadin \
  -DTHRIFT_C11_COMPILER=/path/to/thrift \
  -DTHRIFT_C11_CADIN_ROOT=/path/to/de.cerpsw.cadin.apisrv.parent \
  -DTHRIFT_C11_HTTP_CLIENT=ON \
  -DTHRIFT_C11_WERROR=ON \
  -DBUILD_TESTING=ON
cmake --build out/cadin --parallel
ctest --test-dir out/cadin -R 'cadin|http_auth' --output-on-failure
```

The root path is deliberately opt-in; normal runtime builds do not depend on a
Cadin checkout. All eight IDLs are generated and compiled in the build directory.
The `thrift_c11_cadin_types` object library provides their headers and runtime
dependency to consuming CMake targets. The corpus test links all objects and
checks initialization/cleanup of the former `ExtendedAttributeType` collision.
Building the corpus does not require libcurl; the HTTP client does.

On Windows use the existing vcpkg toolchain and curl package. With a multi-config
generator, add `--config Release` to the build and `-C Release` to CTest; the
executable is then below `examples/cadin/Release`. No POSIX-only client code is
required. Python interoperability tests require Python and this repository's
`lib/py/src` directory.

## Query the API version

`thrift_c11_cadin_client` calls only `StatusSvc.getApiVersion()`. It uses Compact
and Multiplexed protocols and takes the service alias from the generated
`kServiceStatusSvc` IDL constant (`statussvc`). The URL must include the deployed
RPC path, normally `/rpc`.

Provide `THRIFT_CADIN_BEARER_TOKEN` through the process environment, for example
from your local credential mechanism. Then run:

```sh
out/cadin/examples/cadin/thrift_c11_cadin_client \
  --url https://your-cadin-host/rpc \
  --timeout-ms 5000
```

Use `--help` for all options and matching `THRIFT_CADIN_*` environment variables.
Command-line values override environment values. Prefer the environment over
`--bearer-token` to avoid credentials in process arguments and shell history.
No credentials or deployment endpoints are embedded in source or test fixtures.
Use HTTPS for remote tokens; TLS peer and hostname verification stay enabled.
`--ca-file` accepts a private CA bundle. Redirects are not followed.

The executable prints the API version to stdout and lifecycle/error events to
stderr through the runtime logger. A version containing non-printable or non-ASCII
bytes is rejected to prevent terminal control injection. Configuration, transport,
HTTP authorization and RPC failures produce a nonzero exit status. Tokens and
request/response payloads are excluded from logs, even at trace level.

The executable consumes an existing access token. Login, refresh, raw socket TLS,
client certificates, server JWT verification and the Java services' database and
business logic are outside this client integration.

## Regression evidence

The independent Python peer verifies the actual Authorization header, Compact
message, service alias, method, argument shape and reply. It also tests CLI/ENV
precedence, invalid configuration, HTTP 401/403, disabled redirects, malformed or
missing results, and output control characters. It performs no calls to an
external Cadin deployment. Runtime Bearer tests separately verify copied input,
replacement/removal, rejection without changing the previous token, token length
boundaries and absence of tokens in trace output.

Linux validation: all 62 tests pass with ASan/UBSan and warnings as errors when
Zstd, both HTTP options, examples and this integration are enabled. Windows
validation: all 60 tests pass with MSVC 19.44, `/W4 /WX`, the same IDLs and a
native C11 generator. Unix-domain socket tests are excluded on Windows. A live deployment still needs its URL and access token.
Enabling both WebSocket options adds two tests on each platform; an optional C++
WebSocket reference peer adds one further interoperability test.

## Generated descriptor migration

Generated record type descriptors now end in `_type_descriptor` instead of
`_type`, for example `cpro_integra_nodes_extended_attribute_type_descriptor`.
The descriptor and `enum cpro_integra_nodes_extended_attribute_type`
therefore have distinct descriptive names. Regenerate dependent IDLs
together and update hand-written references to those descriptor symbols. Generated
record structs, enum names, client calls and wire encoding are unchanged. The
small regression in `tests/conformance/metadata.thrift` runs even without Cadin.
Other normalized identifier collisions still produce explicit generator errors.
