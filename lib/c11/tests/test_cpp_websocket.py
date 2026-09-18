# SPDX-License-Identifier: Apache-2.0
"""C11 client interoperability with the repository C++ WebSocket server."""
import queue
import subprocess
import sys
import threading

CLIENT_ARGUMENT = 1
SERVER_ARGUMENT = 2
PROCESS_TIMEOUT_SECONDS = 10

for kind in ("binary", "compact"):
    for mux in ("plain", "mux"):
        server = subprocess.Popen([sys.argv[SERVER_ARGUMENT], kind, mux], text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port_queue = queue.Queue()
        threading.Thread(target=lambda: port_queue.put(server.stdout.readline()), daemon=True).start()
        try:
            port = int(port_queue.get(timeout=PROCESS_TIMEOUT_SECONDS))
            client = subprocess.run([sys.argv[CLIENT_ARGUMENT], "--cpp-client",
                                     f"ws://127.0.0.1:{port}/rpc", kind, mux], capture_output=True,
                                    text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            stdout, stderr = server.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
            assert client.returncode == 0, client.stderr
            assert server.returncode == 0, (stdout, stderr)
        finally:
            if server.poll() is None:
                server.kill()
            server.communicate()
