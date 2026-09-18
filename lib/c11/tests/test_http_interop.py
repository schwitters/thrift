# SPDX-License-Identifier: Apache-2.0
"""Independent Python Thrift/HTTP peers and libcurl failure-path checks."""
from http import HTTPStatus
import http.client
import http.server
import importlib.util
import pathlib
import subprocess
import sys
import threading
import time

PROCESS_TIMEOUT_SECONDS = 10
NETWORK_TIMEOUT_SECONDS = 5
EXECUTABLE_ARGUMENT = 1
LIB_DIRECTORY_PARENT = 2

PROBE_BODY_LIMIT = 32
PROBE_SERVER_DELAY_SECONDS = 0.4
SEQUENCE_PROCESS_TIMEOUT_SECONDS = 15
RPC_ITERATIONS = 3
# Mirrors the stable public thrift_status enumeration for the probe process.
STATUS_OK, STATUS_TIMEOUT, STATUS_LIMIT, STATUS_REMOTE = 0, 4, 7, 9

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




def check_server(executable, kind, multiplexed):
    process = subprocess.Popen([executable, "--server", kind, "mux" if multiplexed else "plain"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    connection = None
    try:
        port = int(process.stdout.readline())
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=NETWORK_TIMEOUT_SECONDS)
        protocol_type = TCompactProtocol if kind == "compact" else TBinaryProtocol

        def body(method, value, oneway=False):
            buffer = TMemoryBuffer()
            protocol = protocol_type(buffer)
            name = "Echo:" + method if multiplexed else method
            protocol.writeMessageBegin(name, TMessageType.ONEWAY if oneway else TMessageType.CALL, 23)
            protocol.writeStructBegin("args")
            protocol.writeFieldBegin("value", TType.I32, 1)
            protocol.writeI32(value)
            protocol.writeFieldEnd()
            protocol.writeFieldStop()
            protocol.writeStructEnd()
            protocol.writeMessageEnd()
            return buffer.getvalue()

        for chunked in (False, True):
            payload = body("notify", 7, True)
            connection.request("POST", "/rpc", [payload] if chunked else payload,
                               {"Content-Type": "application/x-thrift"}, encode_chunked=chunked)
            response = connection.getresponse()
            assert response.status == HTTPStatus.OK and response.read() == b""
            payload = body("inherited", 35)
            connection.request("POST", "/rpc", [payload] if chunked else payload,
                               {"Content-Type": "application/x-thrift"}, encode_chunked=chunked)
            response = connection.getresponse()
            assert response.status == HTTPStatus.OK
            assert response.getheader("Content-Type").startswith("application/x-thrift")
            reply = protocol_type(TMemoryBuffer(response.read()))
            assert reply.readMessageBegin() == ("inherited", TMessageType.REPLY, 23)
            reply.readStructBegin()
            assert reply.readFieldBegin()[1:] == (TType.I32, 0)
            assert reply.readI32() == 42
            reply.readFieldEnd()
            assert reply.readFieldBegin()[1] == TType.STOP
            reply.readStructEnd()
            reply.readMessageEnd()
        # A complete RPC followed by garbage must not be committed as HTTP 200.
        connection.request("POST", "/rpc", body("inherited", 1) + b"garbage")
        response = connection.getresponse()
        assert response.status == HTTPStatus.BAD_REQUEST
        response.read()
        connection.close()
        connection = None
        stdout, stderr = process.communicate("\n", timeout=PROCESS_TIMEOUT_SECONDS)
        assert process.returncode == 0, (stdout, stderr)
    finally:
        if connection:
            connection.close()
        if process.poll() is None:
            process.kill()
        process.communicate()


class Peer(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        assert self.headers["Content-Type"] == "application/x-thrift"
        assert self.headers["Accept"] == "application/x-thrift"
        assert self.rfile.read(int(self.headers["Content-Length"])) == b"abc"
        if self.path == "/timeout":
            time.sleep(PROBE_SERVER_DELAY_SECONDS)
            self.close_connection = True
            return
        status = {"/error": HTTPStatus.INTERNAL_SERVER_ERROR, "/redirect": HTTPStatus.FOUND}.get(self.path, HTTPStatus.OK)
        self.send_response(status)
        self.send_header("Content-Type", "application/x-thrift")
        if self.path == "/redirect":
            self.send_header("Location", "/ok")
        payload = b"x" * (PROBE_BODY_LIMIT + 1) if self.path == "/large" else b"ok"
        if self.path == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"1\r\no\r\n1\r\nk\r\n0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)


def check_client(executable):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        # thrift_status: OK=0, TIMEOUT=4, LIMIT=7, REMOTE=9.
        for path, expected in (("ok", STATUS_OK), ("chunked", STATUS_OK), ("large", STATUS_LIMIT),
                               ("error", STATUS_REMOTE), ("redirect", STATUS_REMOTE), ("timeout", STATUS_TIMEOUT)):
            result = subprocess.run([executable, "--probe", f"http://127.0.0.1:{server.server_port}/{path}"],
                                    text=True, capture_output=True, timeout=NETWORK_TIMEOUT_SECONDS, check=True)
            assert int(result.stdout) == expected, (path, result)
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


def check_oneway_connection(executable, kind, multiplexed):
    """OneWayHTTPTest.cpp regression, using an independent HTTP/Thrift peer."""
    accepted = []
    seen = []
    failures = []
    protocol_type = TCompactProtocol if kind == "compact" else TBinaryProtocol

    class RpcPeer(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        notification = 0

        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                payload = self.rfile.read(int(self.headers["Content-Length"]))
                request = protocol_type(TMemoryBuffer(payload))
                name, message_type, sequence = request.readMessageBegin()
                expected_prefix = "Echo:" if multiplexed else ""
                assert name.startswith(expected_prefix)
                method = name[len(expected_prefix):]
                assert method in ("inherited", "notify")
                assert message_type == (TMessageType.ONEWAY if method == "notify" else TMessageType.CALL)
                request.readStructBegin()
                assert request.readFieldBegin()[1:] == (TType.I32, 1)
                value = request.readI32()
                request.readFieldEnd()
                assert request.readFieldBegin()[1] == TType.STOP
                request.readStructEnd()
                request.readMessageEnd()
                seen.append((method, sequence, value))
                response = TMemoryBuffer()
                if method == "notify":
                    self.notification = value
                else:
                    reply = protocol_type(response)
                    reply.writeMessageBegin(method, TMessageType.REPLY, sequence)
                    reply.writeStructBegin("result")
                    reply.writeFieldBegin("success", TType.I32, 0)
                    reply.writeI32(value + self.notification)
                    reply.writeFieldEnd()
                    reply.writeFieldStop()
                    reply.writeStructEnd()
                    reply.writeMessageEnd()
                body = response.getvalue()
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/x-thrift")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                failures.append(repr(error))
                self.close_connection = True

    class RpcServer(http.server.ThreadingHTTPServer):
        def get_request(self):
            connection, address = super().get_request()
            accepted.append(address)
            return connection, address

    server = RpcServer(("127.0.0.1", 0), RpcPeer)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        result = subprocess.run([executable, "--sequence",
                                 f"http://127.0.0.1:{server.server_port}/rpc", kind,
                                 "mux" if multiplexed else "plain"],
                                capture_output=True, text=True, timeout=SEQUENCE_PROCESS_TIMEOUT_SECONDS)
        assert not failures, failures
        assert result.returncode == 0, result.stderr
        assert len(accepted) == 1, accepted
        assert seen == [("inherited", -(1 << 31), 35)] + [
            ("notify", 0, 7), ("notify", 65536, 8), ("inherited", (1 << 31) - 1, 35)] * RPC_ITERATIONS, seen
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


if __name__ == "__main__":
    for kind in ("binary", "compact"):
        for multiplexed in (False, True):
            check_server(sys.argv[EXECUTABLE_ARGUMENT], kind, multiplexed)
            check_oneway_connection(sys.argv[EXECUTABLE_ARGUMENT], kind, multiplexed)
    check_client(sys.argv[EXECUTABLE_ARGUMENT])
