# SPDX-License-Identifier: Apache-2.0
"""Independent RFC 6455 framing and Python Thrift peers for both C11 directions."""
import base64
import hashlib
import http.server
import importlib.util
import pathlib
import queue
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
from http import HTTPStatus

EXECUTABLE_ARGUMENT = 1
PROCESS_TIMEOUT_SECONDS = 10
NETWORK_TIMEOUT_SECONDS = 3
MESSAGE_LIMIT = 131072
LARGE_PAYLOAD_SIZE = 80000
FRAGMENT_SIZE = 70000
PROBE_DELAY_SECONDS = 0.6
PARTIAL_FRAME_DELAY_SECONDS = 0.02
SERVER_TIMEOUT_SECONDS = 2
SERVER_DEADLINE_TOLERANCE_SECONDS = 1
SERVER_DRIP_INTERVAL_SECONDS = 0.2
ADVERTISED_OVERSIZE_BYTES = 1024 * 1024 * 1024
STATUS_OK, STATUS_IO, STATUS_TIMEOUT, STATUS_EOF, STATUS_PROTOCOL, STATUS_LIMIT, STATUS_REMOTE = 0, 3, 4, 5, 6, 7, 9
CONTINUATION, TEXT, BINARY, CLOSE, PING, PONG = 0, 1, 2, 8, 9, 10
FINAL, MASKED = 0x80, 0x80
CLOSE_NORMAL, CLOSE_PROTOCOL, CLOSE_UNSUPPORTED, CLOSE_TOO_LARGE = 1000, 1002, 1003, 1009
MASK = b"\x12\x34\x56\x78"
WS_KEY = base64.b64encode(b"0123456789abcdef").decode()
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BEARER_TOKEN = "test-websocket-token"
RPC_ITERATIONS = 3
LIB_DIRECTORY_PARENT = 2
sys.dont_write_bytecode = True
runtime = pathlib.Path(__file__).resolve().parents[LIB_DIRECTORY_PARENT] / "py" / "src"
spec = importlib.util.spec_from_file_location("thrift", runtime / "__init__.py",
                                             submodule_search_locations=[str(runtime)])
module = importlib.util.module_from_spec(spec)
sys.modules["thrift"] = module
spec.loader.exec_module(module)
from thrift.Thrift import TMessageType, TType
from thrift.protocol.TBinaryProtocol import TBinaryProtocol
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer


def frame(opcode, payload=b"", final=True, masked=False, reserved=0):
    header = bytes([(FINAL if final else 0) | opcode | reserved])
    flag = MASKED if masked else 0
    if len(payload) < 126:
        header += bytes([flag | len(payload)])
    elif len(payload) <= 65535:
        header += bytes([flag | 126]) + struct.pack("!H", len(payload))
    else:
        header += bytes([flag | 127]) + struct.pack("!Q", len(payload))
    if masked:
        header += MASK
        payload = bytes(value ^ MASK[index % len(MASK)] for index, value in enumerate(payload))
    return header + payload


def exact(stream, size):
    value = stream.read(size)
    if len(value) != size:
        raise EOFError("Truncated WebSocket frame")
    return value


def read_frame(stream, masked):
    first, second = exact(stream, 2)
    assert bool(second & MASKED) == masked
    assert not first & 0x70
    size = second & 0x7f
    if size == 126:
        size, = struct.unpack("!H", exact(stream, 2))
    elif size == 127:
        size, = struct.unpack("!Q", exact(stream, 8))
    assert size <= MESSAGE_LIMIT * 2
    mask = exact(stream, len(MASK)) if masked else None
    payload = exact(stream, size)
    if mask:
        payload = bytes(value ^ mask[index % len(mask)] for index, value in enumerate(payload))
    return first & 0x0f, bool(first & FINAL), payload


def connect(port, path="/rpc", extra="", version="13", key=WS_KEY, upgrade="Upgrade"):
    connection = socket.create_connection(("127.0.0.1", port), timeout=NETWORK_TIMEOUT_SECONDS)
    stream = connection.makefile("rb")
    connection.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                        f"Connection: {upgrade}\r\nSec-WebSocket-Version: {version}\r\n"
                        f"Sec-WebSocket-Key: {key}\r\n{extra}\r\n").encode())
    status = int(stream.readline().split()[1])
    headers = {}
    while True:
        line = stream.readline()
        if line == b"\r\n": break
        assert line
        key, value = line.decode().split(":", 1)
        headers[key.lower()] = value.strip()
    if status == HTTPStatus.SWITCHING_PROTOCOLS:
        assert headers["sec-websocket-accept"] == base64.b64encode(
            hashlib.sha1((WS_KEY + WS_GUID).encode()).digest()).decode()
    return connection, stream, status


def rpc_payload(protocol_type, multiplexed, method="inherited", value=35, padding=0):
    buffer = TMemoryBuffer()
    protocol = protocol_type(buffer)
    protocol.writeMessageBegin(("Echo:" if multiplexed else "") + method,
                               TMessageType.ONEWAY if method == "notify" else TMessageType.CALL, 23)
    protocol.writeStructBegin("args")
    protocol.writeFieldBegin("value", TType.I32, 1)
    protocol.writeI32(value)
    protocol.writeFieldEnd()
    if padding:
        protocol.writeFieldBegin("unknown", TType.STRING, 2)
        protocol.writeBinary(b"x" * padding)
        protocol.writeFieldEnd()
    protocol.writeFieldStop()
    protocol.writeStructEnd()
    protocol.writeMessageEnd()
    return buffer.getvalue()


def check_reply(stream, protocol_type):
    opcode, final, payload = read_frame(stream, False)
    assert opcode == BINARY and final
    protocol = protocol_type(TMemoryBuffer(payload))
    assert protocol.readMessageBegin() == ("inherited", TMessageType.REPLY, 23)
    protocol.readStructBegin()
    assert protocol.readFieldBegin()[1:] == (TType.I32, 0)
    assert protocol.readI32() == 42
    protocol.readFieldEnd()
    assert protocol.readFieldBegin()[1] == TType.STOP
    protocol.readStructEnd()
    protocol.readMessageEnd()


def check_server(executable, kind, multiplexed):
    process = subprocess.Popen([executable, "--server", kind, "mux" if multiplexed else "plain"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    started = queue.Queue()
    threading.Thread(target=lambda: started.put(process.stdout.readline()), daemon=True).start()
    try:
        port = int(started.get(timeout=PROCESS_TIMEOUT_SECONDS))
        protocol_type = TCompactProtocol if kind == "compact" else TBinaryProtocol
        connection, stream, status = connect(port)
        try:
            assert status == HTTPStatus.SWITCHING_PROTOCOLS
            connection.sendall(frame(BINARY, masked=True))  # Harmless empty message.
            connection.sendall(frame(BINARY, rpc_payload(protocol_type, multiplexed, "notify", 7), masked=True))
            payload = rpc_payload(protocol_type, multiplexed)
            # FIN belongs to the whole message, with interleaved control traffic.
            connection.sendall(frame(BINARY, payload[:1], final=False, masked=True))
            connection.sendall(frame(PING, b"ping", masked=True))
            assert read_frame(stream, False) == (PONG, True, b"ping")
            connection.sendall(frame(CONTINUATION, b"", final=False, masked=True))
            connection.sendall(frame(CONTINUATION, payload[1:], masked=True))
            check_reply(stream, protocol_type)
            connection.sendall(frame(PONG, b"unsolicited", masked=True))
            # Extended 64-bit frame length and an unknown Thrift field.
            connection.sendall(frame(BINARY, rpc_payload(protocol_type, multiplexed,
                                                         padding=LARGE_PAYLOAD_SIZE), masked=True))
            check_reply(stream, protocol_type)
            connection.sendall(frame(CLOSE, struct.pack("!H", CLOSE_NORMAL), masked=True))
            assert read_frame(stream, False) == (CLOSE, True, struct.pack("!H", CLOSE_NORMAL))
        finally:
            stream.close()
            connection.close()
        hostile = [
            (frame(BINARY, rpc_payload(protocol_type, multiplexed)), CLOSE_PROTOCOL),
            (frame(PING, b"unmasked"), CLOSE_PROTOCOL),
            (frame(TEXT, b"text", masked=True), CLOSE_UNSUPPORTED),
            (frame(BINARY, b"bad", masked=True, reserved=0x40), CLOSE_PROTOCOL),
            (frame(CONTINUATION, b"bad", masked=True), CLOSE_PROTOCOL),
            (frame(PING, b"bad", masked=True, final=False), CLOSE_PROTOCOL),
            (frame(PING, b"x" * 126, masked=True), CLOSE_PROTOCOL),
            (frame(CLOSE, b"x", masked=True), CLOSE_PROTOCOL),
            (frame(CLOSE, struct.pack("!H", 1005), masked=True), CLOSE_PROTOCOL),
            (frame(BINARY, b"x" * (MESSAGE_LIMIT + 1), masked=True), CLOSE_TOO_LARGE),
            (frame(BINARY, b"x" * FRAGMENT_SIZE, final=False, masked=True) +
             frame(CONTINUATION, b"x" * FRAGMENT_SIZE, masked=True), CLOSE_TOO_LARGE),
            (frame(BINARY, b"x", final=False, masked=True) + frame(BINARY, b"x", masked=True), CLOSE_PROTOCOL),
            (frame(BINARY, rpc_payload(protocol_type, multiplexed) + b"garbage", masked=True), CLOSE_PROTOCOL),
        ]
        for payload, expected in hostile:
            connection, stream, status = connect(port)
            try:
                assert status == HTTPStatus.SWITCHING_PROTOCOLS
                connection.sendall(payload)
                opcode, final, payload = read_frame(stream, False)
                assert opcode == CLOSE and final and struct.unpack("!H", payload[:2])[0] == expected
            finally:
                stream.close()
                connection.close()
        for path, extra, expected in (("/other", "", HTTPStatus.NOT_FOUND),
                                       ("/rpc", "Sec-WebSocket-Extensions: permessage-deflate\r\n", HTTPStatus.BAD_REQUEST)):
            connection, stream, status = connect(port, path, extra)
            assert status == expected
            stream.close()
            connection.close()
        for arguments, expected in (({"version": "12"}, HTTPStatus.UPGRADE_REQUIRED),
                                    ({"key": "invalid"}, HTTPStatus.BAD_REQUEST),
                                    ({"upgrade": "keep-alive"}, HTTPStatus.UPGRADE_REQUIRED)):
            connection, stream, status = connect(port, **arguments)
            assert status == expected
            stream.close()
            connection.close()
        if kind == "binary" and not multiplexed:
            check_server_bounds(port)
        stdout, stderr = process.communicate("\n", timeout=PROCESS_TIMEOUT_SECONDS)
        assert process.returncode == 0, (stdout, stderr)
    finally:
        if process.poll() is None: process.kill()
        process.communicate()


def check_server_bounds(port):
    # No payload is sent: rejection must happen from the length header alone,
    # before CivetWeb attempts to allocate/read the announced GiB.
    connection, stream, status = connect(port)
    try:
        assert status == HTTPStatus.SWITCHING_PROTOCOLS
        connection.sendall(bytes([FINAL | BINARY, MASKED | 127]) +
                           struct.pack("!Q", ADVERTISED_OVERSIZE_BYTES) + MASK)
        assert read_frame(stream, False) == (CLOSE, True, struct.pack("!H", CLOSE_TOO_LARGE))
    finally:
        stream.close()
        connection.close()

    # Idle, partial base header, extended header, mask, and payload must all
    # release their worker. Progress on a partial payload must not renew time.
    prefixes = [b"", bytes([FINAL | BINARY]), bytes([FINAL | BINARY, MASKED | 127]),
                bytes([FINAL | BINARY, MASKED | 1]) + MASK[:1],
                bytes([FINAL | BINARY, MASKED | 126]) + struct.pack("!H", MESSAGE_LIMIT // 4) + MASK]
    for prefix in prefixes:
        connection, stream, status = connect(port)
        try:
            assert status == HTTPStatus.SWITCHING_PROTOCOLS
            connection.sendall(prefix)
            connection.settimeout(SERVER_TIMEOUT_SECONDS + SERVER_DEADLINE_TOLERANCE_SECONDS)
            started = time.monotonic()
            try:
                assert connection.recv(1) == b""
            except ConnectionResetError:
                pass
            assert time.monotonic() - started < SERVER_TIMEOUT_SECONDS + SERVER_DEADLINE_TOLERANCE_SECONDS
        finally:
            stream.close()
            connection.close()

    connection, stream, status = connect(port)
    stopped = threading.Event()
    def drip():
        while not stopped.wait(SERVER_DRIP_INTERVAL_SECONDS):
            try:
                connection.sendall(b"x")
            except OSError:
                return
    sender = threading.Thread(target=drip)
    try:
        assert status == HTTPStatus.SWITCHING_PROTOCOLS
        connection.sendall(prefixes[-1])
        connection.settimeout(SERVER_TIMEOUT_SECONDS + SERVER_DEADLINE_TOLERANCE_SECONDS)
        sender.start()
        try:
            assert connection.recv(1) == b""
        except ConnectionResetError:
            pass
    finally:
        stopped.set()
        if sender.ident is not None:
            sender.join()
        stream.close()
        connection.close()
    # A timed-out worker can serve a subsequent connection normally.
    connection, stream, status = connect(port)
    try:
        assert status == HTTPStatus.SWITCHING_PROTOCOLS
        connection.sendall(frame(PING, b"worker available", masked=True))
        assert read_frame(stream, False) == (PONG, True, b"worker available")
    finally:
        stream.close()
        connection.close()


def check_clients(executable):
    failures, seen = [], []

    class Peer(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args): pass

        def close_handshake(self):
            opcode, final, payload = read_frame(self.rfile, True)
            assert opcode == CLOSE and final
            self.connection.sendall(frame(CLOSE, payload))

        def do_GET(self):
            try:
                assert self.headers["Authorization"] == "Bearer " + BEARER_TOKEN
                seen.append(self.path)
                if self.path in ("/reject", "/redirect"):
                    self.send_response(HTTPStatus.FORBIDDEN if self.path == "/reject" else HTTPStatus.FOUND)
                    self.send_header("Content-Length", "0")
                    self.send_header("Location", "/redirect-target")
                    self.end_headers()
                    return
                self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", base64.b64encode(hashlib.sha1(
                    (self.headers["Sec-WebSocket-Key"] + WS_GUID).encode()).digest()).decode())
                self.end_headers()
                self.close_connection = True
                self.connection.settimeout(NETWORK_TIMEOUT_SECONDS)
                if self.path.startswith("/rpc/"):
                    _, _, kind, mux = self.path.split("/")
                    protocol_type = TCompactProtocol if kind == "compact" else TBinaryProtocol
                    for _ in range(RPC_ITERATIONS):
                        for method in ("notify", "inherited"):
                            opcode, final, payload = read_frame(self.rfile, True)
                            assert opcode == BINARY and final
                            request = protocol_type(TMemoryBuffer(payload))
                            name, message_type, sequence = request.readMessageBegin()
                            assert name == ("Echo:" if mux == "mux" else "") + method
                            assert message_type == (TMessageType.ONEWAY if method == "notify" else TMessageType.CALL)
                            request.readStructBegin()
                            assert request.readFieldBegin()[1:] == (TType.I32, 1)
                            value = request.readI32()
                            request.readFieldEnd()
                            assert request.readFieldBegin()[1] == TType.STOP
                            if method == "notify":
                                assert value == 7
                                self.connection.sendall(frame(BINARY))
                            else:
                                assert value == 35
                                buffer = TMemoryBuffer()
                                reply = protocol_type(buffer)
                                reply.writeMessageBegin(method, TMessageType.REPLY, sequence)
                                reply.writeStructBegin("result")
                                reply.writeFieldBegin("success", TType.I32, 0)
                                reply.writeI32(42)
                                reply.writeFieldEnd()
                                reply.writeFieldStop()
                                reply.writeStructEnd()
                                reply.writeMessageEnd()
                                payload = buffer.getvalue()
                                self.connection.sendall(frame(BINARY, payload[:1], final=False))
                                self.connection.sendall(frame(PING, b"between fragments"))
                                assert read_frame(self.rfile, True) == (PONG, True, b"between fragments")
                                self.connection.sendall(frame(CONTINUATION, payload[1:]))
                    self.close_handshake()
                    return
                opcode, final, payload = read_frame(self.rfile, True)
                assert opcode == BINARY and final
                assert payload == (b"x" * MESSAGE_LIMIT if self.path == "/send-large" else b"abc")
                if self.path == "/timeout":
                    time.sleep(PROBE_DELAY_SECONDS)
                    return
                if self.path == "/text":
                    self.connection.sendall(frame(TEXT, b"ok"))
                    return
                if self.path == "/oversize":
                    self.connection.sendall(frame(BINARY, b"x" * (MESSAGE_LIMIT + 1)))
                    return
                if self.path == "/fragment-limit":
                    self.connection.sendall(frame(BINARY, b"x" * FRAGMENT_SIZE, final=False) +
                                            frame(CONTINUATION, b"x" * FRAGMENT_SIZE))
                    return
                if self.path == "/eof": return
                if self.path == "/close":
                    self.connection.sendall(frame(CLOSE, struct.pack("!H", CLOSE_NORMAL)))
                    assert read_frame(self.rfile, True)[0] == CLOSE
                    return
                if self.path == "/short":
                    self.connection.sendall(frame(BINARY, b"o"))
                    return
                if self.path == "/fragment-timeout":
                    self.connection.sendall(frame(BINARY, b"o", final=False))
                    time.sleep(PROBE_DELAY_SECONDS)
                    return
                if self.path == "/partial-ping":
                    ping = frame(PING, b"split control payload")
                    self.connection.sendall(ping[:3])
                    time.sleep(PARTIAL_FRAME_DELAY_SECONDS)
                    self.connection.sendall(ping[3:])
                    assert read_frame(self.rfile, True) == (PONG, True, b"split control payload")
                self.connection.sendall(frame(BINARY, b"ok" + (b"x" * LARGE_PAYLOAD_SIZE if self.path == "/receive-large" else b"")))
                self.close_handshake()
            except Exception as error:
                failures.append((self.path, repr(error)))
                self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        for kind in ("binary", "compact"):
            for mux in ("plain", "mux"):
                result = subprocess.run([executable, "--client", f"ws://127.0.0.1:{server.server_port}/rpc/{kind}/{mux}", kind, mux],
                                        capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
                assert result.returncode == 0, result.stderr
                assert BEARER_TOKEN not in result.stdout + result.stderr
        for path, expected in (("ok", STATUS_OK), ("partial-ping", STATUS_OK),
                               ("send-large", STATUS_OK), ("receive-large", STATUS_OK),
                               ("text", STATUS_PROTOCOL), ("oversize", STATUS_LIMIT),
                               ("fragment-limit", STATUS_LIMIT), ("timeout", STATUS_TIMEOUT),
                               ("fragment-timeout", STATUS_TIMEOUT), ("eof", STATUS_EOF),
                               ("close", STATUS_EOF), ("short", STATUS_EOF),
                               ("reject", STATUS_REMOTE), ("redirect", STATUS_REMOTE)):
            result = subprocess.run([executable, "--large" if path == "send-large" else "--probe",
                                     f"ws://127.0.0.1:{server.server_port}/{path}"],
                                    capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            assert result.returncode == 0 and int(result.stdout) == expected, (path, result)
            assert BEARER_TOKEN not in result.stdout + result.stderr
        assert "/redirect-target" not in seen
        assert not failures, failures
    finally:
        server.shutdown()
        worker.join()
        server.server_close()

    # Exercise real TLS with a test-only local CA, including verification failure.
    certificates = pathlib.Path(__file__).with_name("websocket")
    tls_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificates / "localhost.crt", certificates / "localhost.key")
    tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
    tls_worker = threading.Thread(target=tls_server.serve_forever)
    tls_worker.start()
    try:
        url = f"wss://127.0.0.1:{tls_server.server_port}/ok"
        untrusted = subprocess.run([executable, "--probe", url], capture_output=True,
                                   text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        assert untrusted.returncode == 0 and int(untrusted.stdout) == STATUS_IO, untrusted
        trusted = subprocess.run([executable, "--probe", url, str(certificates / "localhost.crt")],
                                 capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        assert trusted.returncode == 0 and int(trusted.stdout) == STATUS_OK, trusted
        assert not failures, failures
    finally:
        tls_server.shutdown()
        tls_worker.join()
        tls_server.server_close()


if __name__ == "__main__":
    for kind in ("binary", "compact"):
        for multiplexed in (False, True):
            check_server(sys.argv[EXECUTABLE_ARGUMENT], kind, multiplexed)
    check_clients(sys.argv[EXECUTABLE_ARGUMENT])
