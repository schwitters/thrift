# SPDX-License-Identifier: Apache-2.0
"""Exercise real TCP and independent Thrift Binary protocol peers."""
import contextlib
import importlib.util
import pathlib
import socket
import struct
import subprocess
import sys

PROCESS_TIMEOUT_SECONDS = 10
NETWORK_TIMEOUT_SECONDS = 5
NETWORK_TIMEOUT_MILLISECONDS = 5000
EXECUTABLE_ARGUMENT = 1
LIB_DIRECTORY_PARENT = 2



@contextlib.contextmanager
def server(executable, protocol="binary", service=None):
    command = [executable, "--server", "0", protocol]
    if service:
        command.append(service)
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        line = process.stdout.readline()
        if not line.startswith("Listening on "):
            raise RuntimeError(f"Server did not start: {line!r} {process.stderr.read()}")
        yield int(line.split()[-1])
        stdout, stderr = process.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
        if process.returncode:
            raise RuntimeError(f"Server failed: {stdout} {stderr}")
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate()


def recv_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        data = connection.recv(size - len(result))
        if not data:
            raise RuntimeError("Unexpected EOF")
        result.extend(data)
    return bytes(result)


def check_python_runtime(executable):
    # Load this checkout's Python runtime without installing another dependency.
    runtime = pathlib.Path(__file__).resolve().parents[LIB_DIRECTORY_PARENT] / "py" / "src"
    if not runtime.is_dir():
        return
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("thrift", runtime / "__init__.py",
                                                 submodule_search_locations=[str(runtime)])
    module = importlib.util.module_from_spec(spec)
    sys.modules["thrift"] = module
    spec.loader.exec_module(module)
    from thrift.Thrift import TMessageType, TType
    from thrift.protocol.TBinaryProtocol import TBinaryProtocol
    from thrift.protocol.TCompactProtocol import TCompactProtocol
    from thrift.protocol.TMultiplexedProtocol import TMultiplexedProtocol
    from thrift.transport.TSocket import TSocket

    for protocol_name, service_name in [("binary", None), ("compact", None),
                                         ("binary", "Calculator"), ("compact", "Calculator")]:
        for method, left, right, reply_type, result_id in [
            ("add", 20, 22, TMessageType.REPLY, 0),
            ("add", 2**63 - 1, 1, TMessageType.REPLY, 1),
            ("missing", 20, 22, TMessageType.EXCEPTION, 2),
        ]:
            with server(executable, protocol_name, service_name) as port:
                transport = TSocket("127.0.0.1", port)
                transport.setTimeout(NETWORK_TIMEOUT_MILLISECONDS)
                transport.open()
                try:
                    protocol = TCompactProtocol(transport) if protocol_name == "compact" else TBinaryProtocol(transport, strictRead=True, strictWrite=True)
                    if service_name:
                        protocol = TMultiplexedProtocol(protocol, service_name)
                    protocol.writeMessageBegin(method, TMessageType.CALL, -7)
                    protocol.writeStructBegin("args")
                    for field_id, value in [(1, left), (2, right)]:
                        protocol.writeFieldBegin("argument", TType.I64, field_id)
                        protocol.writeI64(value)
                        protocol.writeFieldEnd()
                    # A newer client's extra container field must be skipped.
                    protocol.writeFieldBegin("extra", TType.LIST, 17)
                    protocol.writeListBegin(TType.STRING, 1)
                    protocol.writeString("forward compatible")
                    protocol.writeListEnd()
                    protocol.writeFieldEnd()
                    protocol.writeFieldStop()
                    protocol.writeStructEnd()
                    protocol.writeMessageEnd()
                    transport.flush()
                    if protocol.readMessageBegin() != (method, reply_type, -7):
                        raise RuntimeError("Invalid Python interoperability message header")
                    protocol.readStructBegin()
                    _, field_type, field_id = protocol.readFieldBegin()
                    if field_id != result_id:
                        raise RuntimeError("Unexpected result field")
                    if result_id == 0:
                        if field_type != TType.I64 or protocol.readI64() != 42:
                            raise RuntimeError("Incorrect addition result")
                    elif result_id == 1:
                        if field_type != TType.STRUCT:
                            raise RuntimeError("Missing declared exception")
                        protocol.readStructBegin()
                        _, error_type, error_id = protocol.readFieldBegin()
                        if error_type != TType.STRING or error_id != 1 or protocol.readString() != "integer overflow":
                            raise RuntimeError("Incorrect declared exception")
                        protocol.readFieldEnd()
                        if protocol.readFieldBegin()[1] != TType.STOP:
                            raise RuntimeError("Unterminated declared exception")
                        protocol.readStructEnd()
                    else:
                        if field_type != TType.I32 or protocol.readI32() != 1:
                            raise RuntimeError("Incorrect unknown-method exception")
                    protocol.readFieldEnd()
                    if protocol.readFieldBegin()[1] != TType.STOP:
                        raise RuntimeError("Unterminated result")
                    protocol.readStructEnd()
                    protocol.readMessageEnd()
                finally:
                    transport.close()

    # Validate generated C11 clients against the existing Python server codecs.
    for protocol_name, service_name in [("binary", None), ("compact", None),
                                         ("binary", "Calculator"), ("compact", "Calculator")]:
        with socket.socket() as listener:
            listener.settimeout(NETWORK_TIMEOUT_SECONDS)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            command = [executable, "--client", str(listener.getsockname()[1]), protocol_name]
            if service_name:
                command.append(service_name)
            client = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                peer, _ = listener.accept()
                transport = TSocket()
                transport.setHandle(peer)
                transport.setTimeout(NETWORK_TIMEOUT_MILLISECONDS)
                try:
                    protocol = TCompactProtocol(transport) if protocol_name == "compact" else TBinaryProtocol(transport)
                    expected_name = f"{service_name}:add" if service_name else "add"
                    if protocol.readMessageBegin() != (expected_name, TMessageType.CALL, 1):
                        raise RuntimeError("Invalid generated client message header")
                    protocol.readStructBegin()
                    for field_id, value in [(1, 20), (2, 22)]:
                        _, actual_type, actual_id = protocol.readFieldBegin()
                        if actual_id != field_id or actual_type != TType.I64 or protocol.readI64() != value:
                            raise RuntimeError("Invalid generated client arguments")
                        protocol.readFieldEnd()
                    if protocol.readFieldBegin()[1] != TType.STOP:
                        raise RuntimeError("Unterminated generated client arguments")
                    protocol.readStructEnd()
                    protocol.readMessageEnd()
                    protocol.writeMessageBegin("add", TMessageType.REPLY, 1)
                    protocol.writeStructBegin("result")
                    protocol.writeFieldBegin("success", TType.I64, 0)
                    protocol.writeI64(42)
                    protocol.writeFieldEnd()
                    protocol.writeFieldStop()
                    protocol.writeStructEnd()
                    protocol.writeMessageEnd()
                    transport.flush()
                finally:
                    transport.close()
                stdout, stderr = client.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
                if client.returncode or stdout != "Result: 42\n":
                    raise RuntimeError(f"Generated client failed: {stdout} {stderr}")
            finally:
                if client.poll() is None:
                    client.kill()
                client.communicate()


def main():
    executable = sys.argv[EXECUTABLE_ARGUMENT]
    check_python_runtime(executable)
    for protocol_name, service_name in [("binary", None), ("compact", None),
                                         ("binary", "Calculator"), ("compact", "Calculator")]:
        with server(executable, protocol_name, service_name) as port:
            command = [executable, "--client", str(port), protocol_name]
            if service_name:
                command.append(service_name)
            result = subprocess.run(command, capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS, check=True)
            if result.stdout != "Result: 42\n":
                raise RuntimeError(result.stdout)

    # An independently encoded client catches self-consistent wire-format bugs.
    with server(executable) as port, socket.create_connection(("127.0.0.1", port), timeout=NETWORK_TIMEOUT_SECONDS) as peer:
        request = (struct.pack("!II", 0x80010001, 3) + b"add" + struct.pack("!i", -7)
                   + struct.pack("!bhq", 10, 1, 20) + struct.pack("!bhq", 10, 2, 22) + b"\0")
        peer.sendall(request)
        expected = (struct.pack("!II", 0x80010002, 3) + b"add" + struct.pack("!i", -7)
                    + struct.pack("!bhq", 10, 0, 42) + b"\0")
        received = recv_exact(peer, len(expected))
        if received != expected:
            raise RuntimeError(f"Unexpected wire reply: {received.hex()}")

    # Conversely, an independent server validates the generated C11 client's call.
    with socket.socket() as listener:
        listener.settimeout(NETWORK_TIMEOUT_SECONDS)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        client = subprocess.Popen([executable, "--client", str(listener.getsockname()[1])],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(NETWORK_TIMEOUT_SECONDS)
                expected = (struct.pack("!II", 0x80010001, 3) + b"add" + struct.pack("!i", 1)
                            + struct.pack("!bhq", 10, 1, 20) + struct.pack("!bhq", 10, 2, 22) + b"\0")
                if recv_exact(peer, len(expected)) != expected:
                    raise RuntimeError("Unexpected generated client request")
                peer.sendall(struct.pack("!II", 0x80010002, 3) + b"add" + struct.pack("!i", 1)
                             + struct.pack("!bhq", 10, 0, 42) + b"\0")
            stdout, stderr = client.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
            if client.returncode or stdout != "Result: 42\n":
                raise RuntimeError(f"Client failed: {stdout} {stderr}")
        finally:
            if client.poll() is None:
                client.kill()
            client.communicate()


if __name__ == "__main__":
    main()
