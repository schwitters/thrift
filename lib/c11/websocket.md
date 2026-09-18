# WebSocket transports

The libcurl client and CivetWeb server carry Binary or Compact Thrift RPCs in
binary WebSocket messages. Multiplexed protocols/processors work unchanged on
these transports. C APIs retain the `thrift_` namespace and live under
`transport/` and `server/`.

## Build and dependencies

Both features are opt-in and independent of the HTTP transport switches:

```sh
cmake -S lib/c11 -B out/c11 \
  -DTHRIFT_C11_COMPILER=/path/to/thrift \
  -DTHRIFT_C11_WEBSOCKET_CLIENT=ON \
  -DTHRIFT_C11_WEBSOCKET_SERVER=ON \
  -DTHRIFT_C11_WERROR=ON -DBUILD_TESTING=ON
cmake --build out/c11 --parallel
ctest --test-dir out/c11 -R websocket --output-on-failure
```

The client requires libcurl **8.14 or newer with WebSocket support compiled in**.
It uses CONNECT_ONLY mode, curl_ws_send/recv, and explicit Ping handling with
CURLWS_NOAUTOPONG so partial control payloads are assembled before replying.
The server requires CivetWeb 1.16 with `CIVETWEB_ENABLE_WEBSOCKETS=ON` and the
[bounded-reader patch or vcpkg overlay](dependencies/civetweb/README.md). CMake
rejects an unpatched server dependency. The CMake package exports `thrift_c11_WEBSOCKET_CLIENT` and
`thrift_c11_WEBSOCKET_SERVER` capability booleans and the corresponding dependency
requirements. A core build still requires neither dependency.

Windows vcpkg installation:

```powershell
vcpkg install 'curl[websockets]:x64-windows-static' civetweb:x64-windows-static `
  --overlay-ports=lib/c11/dependencies/civetweb/overlay
```

The default curl package can lack WebSockets even when curl_ws_send/recv symbols
exist. With `x64-windows-static`, select the matching MSVC static runtime as in the
main README. MSVC 19.35 or newer is required for the WebSocket server; CMake enables
`/experimental:c11atomics` for its startup synchronization. The POSIX and Windows
monotonic clocks are separate source files. Socket readiness uses libcurl's
portable polling API, without platform-specific socket types in public headers.

## Client lifecycle

Include `<thrift/c11/transport/thrift_websocket_client.h>`.

1. Initialize libcurl globally, before starting workers. Either the WebSocket
   library init/cleanup pair, the HTTP client's pair, or application-managed
   libcurl lifecycle is sufficient; do not clean up while either kind of client
   is alive.
2. Set `thrift_websocket_client_options`: a `ws://` or `wss://` URL, positive
   `max_message_size` and `timeout_ms`, plus optional `ca_file` and `bearer_token`.
   Bounds must fit `INT_MAX`. The token uses the same RFC 6750 validation and
   length bound as HTTP Bearer authentication.
3. `thrift_websocket_client_create` performs the upgrade and returns an owned
   client plus a borrowed `thrift_transport`. The options' strings are copied or
   consumed before returning. Redirects are disabled; TLS peer and hostname
   verification remain enabled. A non-101 HTTP response returns `THRIFT_REMOTE`.
4. Initialize the desired protocol or multiplexed protocol with the returned
   transport and use the normal generated synchronous client functions.
5. `thrift_websocket_client_close` sends Close and waits for a valid peer Close,
   bounded by the configured timeout. It does not flush pending application data.
   Then call `thrift_websocket_client_destroy`. Destroy alone releases the socket
   immediately and does not perform a graceful Close exchange.

Exclusive access is required per instance, including its transport views.
Independent connections can be used concurrently. Tokens, URLs and message
payloads are excluded from runtime logs. Authenticate at upgrade time; changing
credentials requires a new connection.

Each nonempty `flush` sends one binary message and does **not** wait for a reply.
The first read buffers one complete reply, reassembling continuation frames while
servicing Ping/Pong/Close. Empty binary messages are ignored for compatibility
with peers that flush empty responses. Reads never cross message boundaries.
Oneway calls therefore need no synthetic response and can precede normal calls
on the same connection. Calls are sequential; pipelining, asynchronous callbacks
and unsolicited application notifications are outside this synchronous API.

The timeout bounds upgrade, each complete response reception, flush and Close
exchange. Progress and control traffic do not reset a receive deadline. An
incomplete message returns `THRIFT_EOF`, excessive payloads return `THRIFT_LIMIT`,
and text or malformed frames return a protocol/transport error. After an error,
destroy the connection; there is no automatic retry that could repeat an RPC.

## Server lifecycle

Include `<thrift/c11/server/thrift_websocket_server.h>`.

Initialize CivetWeb globally, then call `thrift_websocket_server_start` with:

- One plain listener, e.g. `127.0.0.1:9090` or `127.0.0.1:0` for a dynamic port.
- An exact upgrade path, normally `/rpc`.
- A positive accumulated request/response message bound and request/idle timeout.
- A worker count from 1 to 64, the Binary/Compact protocol kind and optional
  copied protocol limits.
- A synchronous processor callback and borrowed application context.

Query the actual port with `thrift_websocket_server_port`. Stop joins workers and
releases all connection state before returning. Do not stop from a processor
callback. Keep the handler alive until stop returns, and synchronize shared
handler state when more than one worker is configured. A persistent connection
occupies one worker; a single worker serializes connections as well as RPCs.

Frames are accumulated per connection until FIN. A complete nonempty binary
message invokes one processor call. Responses are held until the processor
returns successfully and consumes the entire request, preventing a successful
wire response for trailing garbage. This does not roll back application side
effects. Oneway RPCs send no response. Ping/Pong/Close are handled outside Thrift;
text frames are rejected with Close 1003. Protocol and size failures close the
connection rather than reusing partial state. Unknown HTTP paths and non-upgrade
requests are rejected; this server exposes no static-file handler.

The listener provides `ws://`. Use a TLS/authentication reverse proxy for `wss://`
server deployments. The client itself supports `wss://`, including a private CA.
Native server TLS, Origin policies, JWT verification, compression extensions and
WebSocket text/JSON payloads are not implemented by these APIs. The server rejects
extension requests rather than enabling a dependency's experimental compression.

### CivetWeb framing boundary

The required CivetWeb extension enforces client masking and rejects excessive
frame lengths before allocating or reading the payload. `max_message_size` bounds
both individual dependency frame allocations and our accumulated request/response
buffers (not the sum of all buffers or allocator overhead). A monotonic deadline
covers idle time and each complete frame, including its header and masking key;
partial progress does not renew it. Expiry closes the connection and releases the
worker. Complete frames reset this deadline; fragmented-message duration and
handler execution are not bounded by it. See the [dependency instructions](dependencies/civetweb/README.md)
for reproducible Linux/CMake and Windows/vcpkg builds.

## C++ reference and tests

The reference is `lib/cpp/src/thrift/transport/TWebSocketServer.h`, specifically
`TWebSocketServer<true>` / `TBinaryWebSocketServerTransportFactory`. Like C++, the
C11 transport buffers a Thrift write and emits a binary WebSocket message on
flush, without an extra Thrift framed-transport length prefix. Both sides handle
Ping outside the Thrift protocol. C11 additionally enforces one complete RPC per
message and assembles continuation frames before dispatch. C++'s text factory
belongs to text protocols and is not used with our Binary/Compact codecs.

`test_websocket.c` checks C/C RPCs for Binary/Compact and plain/multiplexed service
names, repeated oneway calls, normal Close and invalid options. Independent Python
peers check both directions, fragmentation with interleaved Ping, partial control
payloads, empty messages, extended frame lengths, request/response size limits,
malformed messages, trailing bytes, HTTP upgrade errors, redirects, EOF and
whole-message client timeouts. Server regressions cover unmasked frames,
oversized length headers without payload, idle/partial-frame expiry and slow
partial progress. WSS tests check explicit CA trust and rejection without
that CA. Test-only certificate fixtures are under `tests/websocket/`.

`test_cpp_websocket_peer.cc` directly instantiates this checkout's unmodified
C++ transport; the C11 client performs repeated oneway and normal RPCs against it
in all four protocol/multiplexing combinations. The C++ reference ends the socket
without a graceful Close handshake; the reference test checks RPC compatibility
and destroys the client rather than claiming a successful Close exchange.

When the C++ `thrift` target and OpenSSL are present in a repository test build,
CMake builds and registers this reference test automatically. Standalone builds
can instead supply `THRIFT_C11_CPP_WEBSOCKET_PEER=/path/to/prebuilt/reference`.
The peer source links to the repository C++ Thrift library and OpenSSL Crypto.

API references used for the libcurl adapter:
[curl_ws_send](https://curl.se/libcurl/c/curl_ws_send.html),
[curl_ws_recv](https://curl.se/libcurl/c/curl_ws_recv.html), and
[WebSocket interface](https://curl.se/libcurl/c/libcurl-ws.html).
