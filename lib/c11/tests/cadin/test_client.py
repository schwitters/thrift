# SPDX-License-Identifier: Apache-2.0
"""Read-only Cadin client against an independent Compact/Multiplexed HTTP peer."""
import http.server
import importlib.util
import os
import pathlib
import subprocess
import sys
import threading
from http import HTTPStatus

PROCESS_TIMEOUT_SECONDS = 5
EXECUTABLE_ARGUMENT = 1
TEST_TOKEN = "local-test-token"
API_VERSION = "test-api-1.0"
STATUS_SERVICE = "statussvc"
METHOD = "getApiVersion"
LIB_DIRECTORY_PARENT = 3
sys.dont_write_bytecode = True
runtime = pathlib.Path(__file__).resolve().parents[LIB_DIRECTORY_PARENT] / "py" / "src"
spec = importlib.util.spec_from_file_location("thrift", runtime / "__init__.py",
                                             submodule_search_locations=[str(runtime)])
module = importlib.util.module_from_spec(spec)
sys.modules["thrift"] = module
spec.loader.exec_module(module)
from thrift.Thrift import TMessageType, TType
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer


def main():
    executable = sys.argv[EXECUTABLE_ARGUMENT]
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("THRIFT_CADIN_") and key != "THRIFT_LOG_LEVEL"}
    failures = []
    seen = []

    def run(arguments=(), overrides=None, success=True):
        env = dict(environment)
        env.update(overrides or {})
        result = subprocess.run([executable, *arguments], env=env, capture_output=True,
                                text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        assert (result.returncode == 0) == success, (result.returncode, result.stderr)
        assert TEST_TOKEN not in result.stdout + result.stderr, "Token leaked"
        return result

    run(["--help"], {"THRIFT_CADIN_TIMEOUT_MS": "bad"})
    for arguments in ([], ["--url"], ["--unknown", "x"]):
        run(arguments, success=False)

    class Peer(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                assert self.headers.get_all("Authorization") == ["Bearer " + TEST_TOKEN]
                assert self.headers["Content-Type"] == "application/x-thrift"
                request = TCompactProtocol(TMemoryBuffer(self.rfile.read(int(self.headers["Content-Length"]))))
                name, message_type, sequence = request.readMessageBegin()
                assert name == STATUS_SERVICE + ":" + METHOD
                assert message_type == TMessageType.CALL
                request.readStructBegin()
                assert request.readFieldBegin()[1] == TType.STOP
                request.readStructEnd()
                request.readMessageEnd()
                seen.append(self.path)
                status = {"/unauthorized": HTTPStatus.UNAUTHORIZED, "/forbidden": HTTPStatus.FORBIDDEN,
                          "/redirect": HTTPStatus.FOUND}.get(self.path, HTTPStatus.OK)
                response = TMemoryBuffer()
                reply = TCompactProtocol(response)
                reply.writeMessageBegin(METHOD, TMessageType.REPLY, sequence)
                reply.writeStructBegin("result")
                if self.path != "/missing":
                    reply.writeFieldBegin("success", TType.STRING, 0)
                    reply.writeString(API_VERSION if self.path != "/control" else "bad\x1b[31m")
                    reply.writeFieldEnd()
                reply.writeFieldStop()
                reply.writeStructEnd()
                reply.writeMessageEnd()
                body = b"invalid" if self.path == "/malformed" else response.getvalue()
                self.send_response(status)
                self.send_header("Content-Type", "application/x-thrift")
                self.send_header("Content-Length", str(len(body)))
                if self.path == "/redirect":
                    self.send_header("Location", "/redirect-target")
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                failures.append(repr(error))
                self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        base = f"http://127.0.0.1:{server.server_port}"
        config = {"THRIFT_CADIN_URL": base + "/rpc", "THRIFT_CADIN_BEARER_TOKEN": TEST_TOKEN,
                  "THRIFT_CADIN_LOG_LEVEL": "trace"}
        assert run(overrides=config).stdout == API_VERSION + "\n"
        # CLI values override conflicting environment entries, including credentials.
        conflicting = dict(config, THRIFT_CADIN_URL="invalid", THRIFT_CADIN_BEARER_TOKEN="invalid",
                           THRIFT_CADIN_TIMEOUT_MS="invalid", THRIFT_CADIN_LOG_LEVEL="invalid")
        assert run(["--url", base + "/rpc", "--bearer-token", TEST_TOKEN,
                    "--timeout-ms", "2000", "--log-level", "info"], conflicting).stdout == API_VERSION + "\n"
        for path in ("unauthorized", "forbidden", "redirect", "missing", "malformed", "control"):
            run(["--url", base + "/" + path], config, success=False)
        requests_before = len(seen)
        for timeout in ("", "0", "-1", "+1", " 1", "1ms", "2147483648", "9" * 40):
            run(["--timeout-ms", timeout], config, success=False)
        for key, value in (("THRIFT_CADIN_BEARER_TOKEN", ""),
                           ("THRIFT_CADIN_BEARER_TOKEN", "bad\r\nInjected: value"),
                           ("THRIFT_CADIN_LOG_LEVEL", "invalid"),
                           ("THRIFT_CADIN_URL", "file:///etc/passwd")):
            run(overrides=dict(config, **{key: value}), success=False)
        assert len(seen) == requests_before
        assert "/redirect-target" not in seen
        assert not failures, failures
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


if __name__ == "__main__":
    main()
