# SPDX-License-Identifier: Apache-2.0
"""Exercise C/Python RPC peers over filesystem Unix-domain sockets on POSIX."""
import contextlib
import importlib.util
import pathlib
import selectors
import socket
import subprocess
import sys
import tempfile

PROCESS_TIMEOUT_SECONDS = 10
NETWORK_TIMEOUT_SECONDS = 3
NETWORK_TIMEOUT_MILLISECONDS = 3000
EXECUTABLE_ARGUMENT = 1
LIB_DIRECTORY_PARENT = 2
REQUEST_VALUE = 42
REQUEST_SEQUENCE = 7
ARGUMENT_FIELD = 1
SUCCESS_FIELD = 0
LISTEN_BACKLOG = 1


@contextlib.contextmanager
def process(command):
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        yield child
        output, errors = child.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
        if child.returncode:
            raise RuntimeError(f"Failed {command}: {output} {errors}")
    finally:
        if child.poll() is None:
            child.kill()
        child.communicate(timeout=PROCESS_TIMEOUT_SECONDS)


@contextlib.contextmanager
def server(command):
    with process(command) as child:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout, selectors.EVENT_READ)
            if not selector.select(PROCESS_TIMEOUT_SECONDS):
                raise RuntimeError("Server startup timed out")
        if child.stdout.readline().strip() != "READY":
            raise RuntimeError("Server failed to announce readiness")
        yield


def main():
    runtime = pathlib.Path(__file__).resolve().parents[LIB_DIRECTORY_PARENT] / "py" / "src"
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

    executable = sys.argv[EXECUTABLE_ARGUMENT]
    for kind in ("binary", "compact"):
        codec = TBinaryProtocol if kind == "binary" else TCompactProtocol
        for multiplexed in (False, True):
            with tempfile.TemporaryDirectory(prefix="thrift-rpc-", dir="/tmp") as directory:
                path = pathlib.Path(directory) / "rpc.sock"
                arguments = [str(path), kind] + (["Base"] if multiplexed else [])
                server_command = [executable, "--server", *arguments]
                client_command = [executable, "--client", *arguments]

                # Both generated C endpoints, including the synchronous server helper.
                with server(server_command):
                    completed = subprocess.run(client_command, capture_output=True, text=True,
                                               timeout=PROCESS_TIMEOUT_SECONDS, check=True)
                    if completed.stdout.strip() != f"RESULT {REQUEST_VALUE}":
                        raise RuntimeError("Unexpected C client result")
                if not path.is_socket():
                    raise RuntimeError("Listener close unexpectedly removed the application-owned path")
                path.unlink()

                # Independent Python client -> C processor.
                with server(server_command):
                    transport = TSocket(unix_socket=str(path))
                    transport.setTimeout(NETWORK_TIMEOUT_MILLISECONDS)
                    transport.open()
                    try:
                        protocol = codec(transport)
                        if multiplexed:
                            protocol = TMultiplexedProtocol(protocol, "Base")
                        protocol.writeMessageBegin("inherited", TMessageType.CALL, REQUEST_SEQUENCE)
                        protocol.writeStructBegin("args")
                        protocol.writeFieldBegin("value", TType.I32, ARGUMENT_FIELD)
                        protocol.writeI32(REQUEST_VALUE)
                        protocol.writeFieldEnd()
                        protocol.writeFieldStop()
                        protocol.writeStructEnd()
                        protocol.writeMessageEnd()
                        transport.flush()
                        if protocol.readMessageBegin() != ("inherited", TMessageType.REPLY, REQUEST_SEQUENCE):
                            raise RuntimeError("Invalid reply header")
                        protocol.readStructBegin()
                        if protocol.readFieldBegin()[1:] != (TType.I32, SUCCESS_FIELD):
                            raise RuntimeError("Invalid result field")
                        if protocol.readI32() != REQUEST_VALUE:
                            raise RuntimeError("Invalid result value")
                        protocol.readFieldEnd()
                        if protocol.readFieldBegin()[1] != TType.STOP:
                            raise RuntimeError("Trailing result field")
                        protocol.readStructEnd()
                        protocol.readMessageEnd()
                    finally:
                        transport.close()
                path.unlink()

                # Generated C client -> independent Python server codec.
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                    listener.settimeout(NETWORK_TIMEOUT_SECONDS)
                    listener.bind(str(path))
                    listener.listen(LISTEN_BACKLOG)
                    with process(client_command):
                        connection, _ = listener.accept()
                        connection.settimeout(NETWORK_TIMEOUT_SECONDS)
                        transport = TSocket()
                        transport.setHandle(connection)
                        try:
                            protocol = codec(transport)
                            method = "Base:inherited" if multiplexed else "inherited"
                            if protocol.readMessageBegin() != (method, TMessageType.CALL, REQUEST_SEQUENCE):
                                raise RuntimeError("Invalid request header")
                            protocol.readStructBegin()
                            if protocol.readFieldBegin()[1:] != (TType.I32, ARGUMENT_FIELD):
                                raise RuntimeError("Invalid argument field")
                            if protocol.readI32() != REQUEST_VALUE:
                                raise RuntimeError("Invalid argument value")
                            protocol.readFieldEnd()
                            if protocol.readFieldBegin()[1] != TType.STOP:
                                raise RuntimeError("Trailing argument")
                            protocol.readStructEnd()
                            protocol.readMessageEnd()
                            protocol.writeMessageBegin("inherited", TMessageType.REPLY, REQUEST_SEQUENCE)
                            protocol.writeStructBegin("result")
                            protocol.writeFieldBegin("success", TType.I32, SUCCESS_FIELD)
                            protocol.writeI32(REQUEST_VALUE)
                            protocol.writeFieldEnd()
                            protocol.writeFieldStop()
                            protocol.writeStructEnd()
                            protocol.writeMessageEnd()
                            transport.flush()
                        finally:
                            transport.close()
                path.unlink()


if __name__ == "__main__":
    main()
