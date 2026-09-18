# SPDX-License-Identifier: Apache-2.0
"""Compare generated C11 codecs with this checkout's Python Thrift runtime."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import uuid

PROCESS_TIMEOUT_SECONDS = 10
EXECUTABLE_ARGUMENT = 1
LIB_DIRECTORY_PARENT = 2



def main():
    sys.dont_write_bytecode = True
    runtime = pathlib.Path(__file__).resolve().parents[LIB_DIRECTORY_PARENT] / "py" / "src"
    spec = importlib.util.spec_from_file_location("thrift", runtime / "__init__.py",
                                                 submodule_search_locations=[str(runtime)])
    module = importlib.util.module_from_spec(spec)
    sys.modules["thrift"] = module
    spec.loader.exec_module(module)
    from thrift.Thrift import TType
    from thrift.protocol.TBinaryProtocol import TBinaryProtocol
    from thrift.protocol.TCompactProtocol import TCompactProtocol
    from thrift.transport.TTransport import TMemoryBuffer

    def record(protocol, fields):
        protocol.writeStructBegin("record")
        for field_id, field_type, value in fields:
            wire = field_type[0] if isinstance(field_type, tuple) else field_type
            protocol.writeFieldBegin("field", wire, field_id)
            write(protocol, field_type, value)
            protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()

    def write(protocol, field_type, value):
        if isinstance(field_type, tuple):
            wire, *parameters = field_type
            if wire == TType.STRUCT:
                record(protocol, value)
            elif wire == TType.MAP:
                key, element = parameters
                key_wire = key[0] if isinstance(key, tuple) else key
                element_wire = element[0] if isinstance(element, tuple) else element
                protocol.writeMapBegin(key_wire, element_wire, len(value))
                for k, v in value:
                    write(protocol, key, k)
                    write(protocol, element, v)
                protocol.writeMapEnd()
            else:
                element = parameters[0]
                element_wire = element[0] if isinstance(element, tuple) else element
                begin = protocol.writeListBegin if wire == TType.LIST else protocol.writeSetBegin
                end = protocol.writeListEnd if wire == TType.LIST else protocol.writeSetEnd
                begin(element_wire, len(value))
                for item in value:
                    write(protocol, element, item)
                end()
            return
        writers = {
            TType.BOOL: protocol.writeBool, TType.BYTE: protocol.writeByte,
            TType.I16: protocol.writeI16, TType.I32: protocol.writeI32,
            TType.I64: protocol.writeI64, TType.DOUBLE: protocol.writeDouble,
            TType.STRING: protocol.writeBinary, TType.UUID: protocol.writeUuid,
        }
        writers[field_type](value)

    packet = [
        (1, TType.BOOL, True), (2, TType.BYTE, -128), (3, TType.I16, -32768),
        (4, TType.I32, -(2**31)), (5, TType.I64, -(2**63)), (6, TType.DOUBLE, -0.0),
        (7, TType.STRING, "snowman \u2603".encode()), (8, TType.STRING, b"a\0b"),
        (9, (TType.LIST, TType.I32), [0, 1, -1, -(2**31), 2**31 - 1] * 4),
        (10, (TType.SET, TType.STRING), [b"alpha", b"beta"]),
        (11, (TType.MAP, TType.STRING, (TType.LIST, TType.I64)),
         [(b"empty", []), (b"long", [-1, -(2**63), 2**63 - 1])]),
        (12, (TType.STRUCT,), [(1, TType.I32, 42), (2, (TType.STRUCT,), [(1, TType.I32, -3)])]),
        (13, TType.I32, 7), (14, (TType.STRUCT,), [(2, TType.STRING, b"selected")]),
        (15, (TType.STRUCT,), [(1, TType.I32, 99)]),
        (16, TType.UUID, uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")),
    ]
    edges = [
        (1, TType.BOOL, True), (2, TType.BOOL, False),
        (20, (TType.LIST, TType.BOOL), [True, False] * 10),
        (3, (TType.MAP, TType.I16, TType.BOOL), [(-32768, False), (32767, True)]),
        (4, (TType.LIST, (TType.LIST, TType.I32)), [[], [-(2**31), 2**31 - 1]]),
        (5, (TType.MAP, TType.STRING, (TType.LIST, TType.BOOL)), []),
        (32767, TType.I16, -32768),
    ]
    unknown = [
        (100, TType.BOOL, False),
        (101, (TType.MAP, TType.BOOL, (TType.LIST, TType.UUID)),
         [(True, [uuid.UUID(int=1)]), (False, [])]),
        (102, (TType.MAP, TType.STRING, TType.BOOL), []),
    ]
    for name, factory in [("binary", TBinaryProtocol), ("compact", TCompactProtocol)]:
        for object_name, fields in [("packet", packet), ("edges", edges)]:
            expected = TMemoryBuffer()
            # Like C++, initialized optional defaults remain present when an
            # older peer omits them and are emitted during reserialization.
            defaults = [(17, (TType.LIST, TType.STRING), [b"first", b"second"])] if object_name == "packet" else []
            record(factory(expected), fields + defaults)
            for extra in [[], unknown]:
                incoming = TMemoryBuffer()
                record(factory(incoming), fields + extra)
                with tempfile.TemporaryDirectory() as directory:
                    source = pathlib.Path(directory) / "input.bin"
                    destination = pathlib.Path(directory) / "output.bin"
                    source.write_bytes(incoming.getvalue())
                    subprocess.run([sys.argv[EXECUTABLE_ARGUMENT], name, object_name, str(source), str(destination)],
                                   check=True, timeout=PROCESS_TIMEOUT_SECONDS)
                    if destination.read_bytes() != expected.getvalue():
                        raise RuntimeError(f"Wire mismatch for {name} {object_name}, unknown={bool(extra)}")


if __name__ == "__main__":
    main()
