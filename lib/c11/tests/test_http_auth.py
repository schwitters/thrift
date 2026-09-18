# SPDX-License-Identifier: Apache-2.0
"""Verify copied/replaced/removed tokens on real HTTP requests; reject log leaks."""
import http.server
import pathlib
import ssl
import subprocess
import sys
import threading
from http import HTTPStatus

PROCESS_TIMEOUT_SECONDS = 15
EXECUTABLE_ARGUMENT = 1
TOKEN_MAX = 16384
FIRST_TOKEN = "test.AZaz09-._~+/=="
SECOND_TOKEN = "replacement-token"


def main():
    seen = []
    failures = []

    class Peer(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                assert self.rfile.read(int(self.headers["Content-Length"])) == b"abc"
                if self.path == "/redirect":
                    self.send_response(HTTPStatus.TEMPORARY_REDIRECT)
                    self.send_header("Location", "/target")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                assert self.path != "/target", "Client followed a redirect"
                seen.append(self.headers.get_all("Authorization"))
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Length", "0")
                self.end_headers()
            except Exception as error:
                failures.append(repr(error))
                self.close_connection = True

    def policy(url, ca, expected):
        result = subprocess.run([sys.argv[EXECUTABLE_ARGUMENT], url, str(ca), expected],
                                capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        assert result.returncode == 0, result.stdout + result.stderr

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        result = subprocess.run([sys.argv[EXECUTABLE_ARGUMENT], f"http://127.0.0.1:{server.server_port}/rpc"],
                                capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        assert result.returncode == 0, result.stderr
        assert not failures, failures
        assert seen == [["Bearer " + FIRST_TOKEN], ["Bearer " + FIRST_TOKEN],
                        ["Bearer " + SECOND_TOKEN], None,
                        ["Bearer " + "x" * TOKEN_MAX], ["Bearer " + "x" * TOKEN_MAX]]
        for token in (FIRST_TOKEN, SECOND_TOKEN, "x" * TOKEN_MAX):
            assert token not in result.stdout + result.stderr, "Token leaked to process output"
        policy(f"http://127.0.0.1:{server.server_port}/redirect", "-", "redirect")
        assert not failures, failures
    finally:
        server.shutdown()
        worker.join()
        server.server_close()

    certificates = pathlib.Path(__file__).resolve().parent / "certificates"
    # Trust the existing self-signed fixture directly. This also works with
    # Schannel's revocation policy without an external CRL service in the test.
    identity = pathlib.Path(__file__).resolve().parent / "websocket"
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(str(identity / "localhost.crt"), str(identity / "localhost.key"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        url = f"https://127.0.0.1:{server.server_port}/rpc"
        policy(url, identity / "localhost.crt", "ok")
        policy(url, certificates / "other-ca.pem", "tls")
        assert not failures, failures
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


if __name__ == "__main__":
    main()
