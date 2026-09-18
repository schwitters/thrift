# Nonblocking TLS sockets and framed server

Enable `THRIFT_C11_TLS=ON`. Linux links OpenSSL (at least 1.1.1) and pthreads.
Windows links only native libraries: Winsock, Schannel/SSPI, CryptoAPI, CNG and
BCrypt. It does not need OpenSSL or a vcpkg TLS package. The Windows implementation
uses `SCH_CREDENTIALS`: Windows 10 1809 / Server 2019 or later and an appropriate
Windows SDK are required. TLS 1.3 availability follows OS policy; TLS 1.2 is the
minimum on both platforms.

```sh
cmake -S lib/c11 -B build-c11-tls -DTHRIFT_C11_TLS=ON \
  -DTHRIFT_C11_COMPILER=/path/to/thrift -DTHRIFT_C11_WERROR=ON
cmake --build build-c11-tls
ctest --test-dir build-c11-tls --output-on-failure
```

With a Visual Studio generator, use `--config Debug` for the build and `-C Debug`
for CTest. The compiler option enables additional generated-RPC/conformance tests;
the raw TLS tests also build without a Thrift compiler.

## API and scheduling

Public headers:

- `transport/thrift_tls_socket.h`: credential contexts, listener/client sockets,
  partial I/O, handshake, readiness polling and optional framed client adapter.
- `server/thrift_tls_server.h`: bounded framed reactor and processor workers.

A socket operation returns `THRIFT_AGAIN` when more network progress is needed.
Keep the socket, poll `thrift_tls_interest(socket)`, and retry the same operation.
Successful reads/writes may transfer fewer bytes than requested: advance by the
returned count. A successful write accepts bytes into bounded TLS output; call
`thrift_tls_flush()` until it succeeds before considering them sent. Network
operations do not wait, except the explicitly requested `thrift_tls_poll()`.
Credential loading and local cryptographic operations still execute synchronously.
Connect uses a numeric IPv4/IPv6 address to avoid blocking DNS; `server_name` is a
separate, required certificate identity. Resolve names before entering the reactor.

The server owns a fixed number of workers and connection slots. The caller drives
`thrift_tls_server_poll()`. TLS negotiation and socket I/O run in that caller;
workers process complete in-memory requests only. Each connection has at most one
active RPC. Queued requests, TLS buffers and frame sizes have finite bounds.
Idle/partial frames and handshakes have deadlines; the RPC deadline includes
worker queuing, processing and sending the reply. A timed-out running handler
retains its slot until it returns, but its socket closes immediately. Destruction
joins the workers; application handlers must eventually return.

The current readiness backends use POSIX `poll` / Windows `WSAPoll`, with a
10 ms maximum wait quantum to observe worker completions. This avoids a thread
per connection but is not an epoll/IOCP implementation or a performance claim.
Choose the connection/frame limits to fit the application's memory budget. The
upper bound includes a request and response buffer per active connection, TLS
state, and a fixed worker stack per worker. Handlers and logging sinks must be
safe for concurrent use. Configure logging before creating the workers.

The wire format is the C++ nonblocking server's framed format: a positive 4-byte
big-endian frame length followed by one complete Thrift message. The payload may
use Binary or Compact; multiplexing is applied through the existing processor.
Plain TCP clients without framing are not compatible with this listener.

## Credentials and validation

Linux servers load a PEM certificate chain and PEM private key. Windows servers
load a PKCS#12/PFX file containing the server identity. A password may be supplied
in `thrift_tls_options.password`; the library does not retain or log it. Pass
NULL for `private_key_file` on Windows. `ca_file` is client-only: a PEM CA bundle
on Linux, or one DER CA certificate on Windows. NULL selects platform roots.
The custom Windows CA is isolated in a private chain engine and is not installed
into a system trust store.

Windows imports the PFX transiently, selects its private-key certificate, then
copies that key into a randomly named CNG container for Schannel. Existing keys
are never overwritten. `machine_key=false` uses user key storage; server/service
accounts may select `machine_key=true` to use machine key storage. That account
must have the corresponding key-storage permissions. In particular, an SSH
public-key logon may lack the credentials needed to unlock user DPAPI storage;
the Windows test host uses machine key storage. Linux and client contexts reject
`machine_key=true`.

Normal context destruction deletes the owned CNG container after freeing the
Schannel credential. A forced process termination can leave an orphaned key
container; this is an operational limitation of persistent Windows key storage.
Certificate stores are not modified. For Windows certificate chains, provision
necessary intermediates in the server's OS certificate environment when required
by Schannel's chain construction; test coverage currently uses a directly trusted
issuing CA.

Clients validate trust, validity period, server-auth usage and DNS/IP identity.
There is no insecure verification switch. Chain construction does not retrieve
certificates over the network in the reactor. Automatic CRL/OCSP revocation
checking, client certificates/mTLS, TLS 0-RTT, ALPN and an asynchronous generated
client API are outside this implementation's scope. OpenSSL disables TLS 1.2
renegotiation; Schannel handles its native post-handshake control messages.

## Using generated clients and processors

Existing generated client calls are synchronous. `thrift_tls_client_create()`
provides their framed `thrift_transport`; it explicitly waits using the
nonblocking socket API. It supports one outstanding RPC and bounded send/receive
waits. Do not call this adapter from the same thread that must drive its server.
For an entirely nonblocking client, use the partial-I/O API and serialize/decode
complete messages in memory, advancing handshake/send/receive states yourself.

A generated processor fits the server through the existing callback convention:

```c
static enum thrift_status process(struct thrift_protocol *protocol, void *handler)
{
    return my_service_process(protocol, handler);
}

struct thrift_tls_server_options options = {0};
options.context = credentials;       /* borrowed server TLS context */
options.max_connections = 128;
options.max_frame_size = 1024 * 1024;
options.timeout_ms = 5000;
options.worker_count = 4;
options.kind = THRIFT_COMPACT;
/* Set address/port, then check thrift_tls_server_create(...).
 * Repeatedly call thrift_tls_server_poll(server, timeout_ms), checking status.
 * Destroy the server before its credentials and handler. */
```

`tests/test_tls_rpc.c` contains a complete working generated-client/server example,
including Binary/Compact, multiplexing and oneway followed by a normal RPC.
`tests/test_tls.c` demonstrates the raw incremental handshake and close_notify.
The independent Python peer tests partial records, concurrent connections,
stalled peers, large/invalid frames, identity/trust/expiry rejection and deadlines.
Test certificates and their private keys are public fixtures, valid until 2040;
`certificates/generate_tls.py` regenerates them using Python cryptography, which
is not a build/runtime dependency. Never use test identities in a deployment.

Implementation references:
[Schannel credentials](https://learn.microsoft.com/en-us/windows/win32/api/schannel/ns-schannel-sch_credentials),
[PFX key lifetime](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-pfximportcertstore),
[CNG key import](https://learn.microsoft.com/en-us/windows/win32/api/ncrypt/nf-ncrypt-ncryptimportkey).
