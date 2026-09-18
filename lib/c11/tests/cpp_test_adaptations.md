# C++ regression scenarios adapted for C11

The C11 tests below derive from the checked-in C++ tests, with additional boundary
cases where useful. They use public C11 APIs, explicit status checks and CTest;
Boost.Test and C++ runtime dependencies are not required. Checks remain active in
Release builds. Existing C11 tests still cover generated ownership/defaults,
recursive values, UUID wire bytes, malformed varints and independent Python codecs.

| C++ source / scenario | C11 test | Coverage and adaptation |
| --- | --- | --- |
| `lib/cpp/test/AllProtocolTests.tcc`: `testProtocol`, `testField` | `test_cpp_protocol.c`, `scalars` | All 256 byte values; signed integer extrema; positive/negative powers of two; representative i16/i32 values; strings including embedded NUL and every byte value; UUID; booleans. Additional signed/extended field IDs and bit-exact doubles (signed zero, subnormal, infinities, quiet NaN). Binary and Compact. |
| `AllProtocolTests.tcc`: `testMessage` | `test_cpp_protocol.c`, `messages` | Original message names and sequence examples, plus negative and extreme sequence IDs. Actual public client/processor calls exercise CALL, REPLY, ONEWAY and application EXCEPTION messages and handler invocation counts. |
| `lib/cpp/test/ThrifttReadCheckTests.cpp`: read-check pass/failure | `test_cpp_protocol.c`, `containers` | List/set/map roundtrips with UUID elements, map keys, sizes 0/1/14/15/16/127/128/255/256; byte budget exactly equal to encoded size and one byte below, on reads and writes; container-count limit; unknown-field skipping. Failed reads preserve the destination and release temporary allocations. |
| `ThrifttReadCheckTests.cpp`: Binary/Compact container-size overflow | `test_cpp_protocol.c`, `hostile` | Independently encoded lists advertising `0x40000000` i32 elements or `0x10000000` UUID elements (4 GiB minimum content). Both typed reads and unknown-field skipping reject them with `LIMIT`, with the count limit relaxed so it does not mask the byte-budget check. |
| `lib/cpp/test/OptionalRequiredTest.cpp`: schema compatibility | `test_cpp_protocol.c`, `schema` | Optional sender field present/absent against optional/default/required readers; same-ID wire-type changes; required errors; unchanged destination on failure; absent optional fields replace previously populated values. |
| `lib/cpp/test/TMemoryBufferTest.cpp`: `test_read_write_grow`, `test_observe`, `test_exceptions`, `test_buffer_overflow` | `test_cpp_memory.c` | Power-of-two write/read chunks, binary contents, borrowed storage visibility, append after EOF, exact capacity, zero capacity, zero-length operations and `SIZE_MAX` requests without position/size corruption. C11 preallocates fixed storage; it does not implement the C++ growth/ownership modes. |
| `lib/cpp/test/ZlibTest.cpp`: `test_read_write_mix`, `test_write_after_flush`, `test_no_write` | `test_cpp_zstd.c` | Deterministic compressible and pseudorandom inputs; asymmetric chunk sizes from 1 byte through 128 KiB block boundaries; concatenated frames; reading across frame boundaries; continued use after flush; destruction without implicit flush. Additional injected read/write/flush failures verify status propagation and no retry after failure. |
| `ZlibTest.cpp`: checksum and message-limit scenarios | Existing `test_zstd.c` | Independent libzstd encoder/decoder, every truncation of a frame, checksum corruption, absent content-size field, compressed/decompressed limits and empty-frame limit. Kept rather than duplicated. |
| `lib/cpp/test/OneWayHTTPTest.cpp`: `JSON_HTTP_OneWayWrapperDoesNotPoisonNextCall` | `test_http.c`, `--sequence`; `test_http_interop.py`, `check_oneway_connection` | An independent Python HTTP/Thrift server observes an ordinary RPC, repeated pairs of Oneway RPCs and ordinary replies. Checks exact call order, sequence IDs, empty Oneway responses and exactly one accepted TCP connection. Adapted to Binary and Compact, each with and without multiplexing. |

## Intentional differences

- C11 transports transfer exactly the requested number of bytes or return an
  explicit error. The C++ partial-read loops therefore cap the final C11 read to
  the remaining data. EOF and rejected writes leave memory-buffer positions intact.
- C11 exposes record descriptors and RPC methods rather than public naked scalar
  protocol methods. Scalar tests use one-field records; message tests use real
  client/processor dispatch. No private protocol vtable is accessed.
- C11 deserialization is transactional. Schema tests check that failure preserves
  existing values; they do not copy C++ object mutation or copy-constructor rules.
- C++ read-check tests estimate minimum wire lengths early. C11 also enforces a
  cumulative byte budget. Tests verify acceptance at the exact serialized size
  and rejection below it without requiring identical internal failure positions.
- Zstd is not Zlib: each C11 flush ends a complete standard Zstandard frame and
  later writes begin another. An empty final flush can leave an empty frame for
  the next read. Zlib `finish()`/`verifyChecksum()` API tests cannot be copied
  literally; C11 validates frame checksums before exposing decoded data.
- The C11 HTTP client completes a POST during flush. C++ tests that delay socket
  writes, batch raw HTTP requests, or use JSON are not applicable. The relevant
  Oneway/next-reply and persistent-connection regression is tested independently.
- SSL socket, JSON/Header, file/pipe, nonblocking/thread-pool server, Qt and C++
  language-specific tests remain outside C11's implemented feature set.

## Running

Configure the C11 project normally, then run `ctest --test-dir <build>` (add
`-C Release` for Visual Studio). The `thrift_c11_cpp_*` tests are individually
selectable with `-R thrift_c11_cpp_`. Protocol and memory tests work without the
compiler or optional libraries. Zstd groups require `THRIFT_C11_ZSTD=ON`.
HTTP interoperability requires both HTTP options, a native compiler supporting
`--gen c11`, Python and this checkout's `lib/py/src`.

With Cadin and both WebSocket options, the configuration contains 64 CTest entries
on Linux (62 on Windows), plus one optional test against the actual C++ WebSocket
transport. These include 14 adapted
entries, the extended HTTP interoperability test, two logging tests and 28
[IDL conformance entries](conformance/README.md). Two additional POSIX tests cover
Unix-domain socket lifecycle and generated RPC/Python interoperability.
The [WebSocket guide](../websocket.md) describes the independent frame-level
tests and `test_cpp_websocket_peer.cc`, which instantiates the unmodified C++
`TWebSocketServer<true>` for Binary/Compact and multiplexed RPC comparisons.
Golden wire bytes and representative protocol values remain literal test data;
buffer sizes, timeouts, repeat counts and other tuning parameters are named.
