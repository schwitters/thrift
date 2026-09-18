# SPDX-License-Identifier: Apache-2.0
"""Independent Python ssl peers exercise native OpenSSL and Schannel paths."""
import concurrent.futures
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

EXE = sys.argv[1]
CERTS = Path(__file__).resolve().parent / 'certificates'
WINDOWS = sys.platform == 'win32'
VERSIONS = [ssl.TLSVersion.TLSv1_2]
if ssl.HAS_TLSv1_3 and (not WINDOWS or sys.getwindowsversion().build >= 20348):
    VERSIONS.append(ssl.TLSVersion.TLSv1_3)
CA = CERTS / ('ca.der' if WINDOWS else 'ca.pem')
TIMEOUT = 4
FRAME_LIMIT = 256 * 1024
BINARY = bytes.fromhex('0800018000000000')
COMPACT = bytes.fromhex('15ffffffff0f00')


def exact(connection, count):
    result = bytearray()
    while len(result) < count:
        data = connection.recv(count - len(result))
        if not data:
            raise EOFError('unexpected connection end')
        result.extend(data)
    return bytes(result)


def tls_client(port, version=None):
    context = ssl.create_default_context(cafile=str(CERTS / 'ca.pem'))
    if version is not None:
        context.minimum_version = context.maximum_version = version
    raw = socket.create_connection(('127.0.0.1', port), TIMEOUT)
    try:
        return context.wrap_socket(raw, server_hostname='localhost')
    except BaseException:
        raw.close()
        raise


def rpc(connection, payload, fragmented=False):
    wire = struct.pack('!I', len(payload)) + payload
    if fragmented:
        for byte in wire:
            connection.sendall(bytes([byte]))
    else:
        connection.sendall(wire)
    length, = struct.unpack('!I', exact(connection, 4))
    assert 0 < length <= FRAME_LIMIT
    return exact(connection, length)


def server_cases(kind):
    certificate = CERTS / ('server.pfx' if WINDOWS else 'server.pem')
    key = '-' if WINDOWS else str(CERTS / 'server-key.pem')
    process = subprocess.Popen([EXE, 'server', str(certificate), key, kind],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = process.stdout.readline()
        assert line, process.stderr.read()
        port = int(line)
        payload = BINARY if kind == 'binary' else COMPACT
        # A peer that never starts TLS must not consume a processor worker.
        with socket.create_connection(('127.0.0.1', port), TIMEOUT) as stalled:
            for version in VERSIONS:
                with tls_client(port, version) as connection:
                    assert rpc(connection, payload, True) == payload
                    assert rpc(connection, payload) == payload  # persistent connection
            def concurrent_rpc(_):
                with tls_client(port) as connection:
                    assert rpc(connection, payload) == payload
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(concurrent_rpc, range(16)))
            if kind == 'binary':
                # Unknown large field tests multiple TLS records and bounded framing.
                large = payload[:-1] + b'\x0b\x00\x02' + struct.pack('!I', 180000) + b'x' * 180000 + b'\0'
                with tls_client(port) as connection:
                    assert rpc(connection, large) == payload
            for header in (0, FRAME_LIMIT + 1, 0xffffffff):
                with tls_client(port) as connection:
                    connection.sendall(struct.pack('!I', header))
                    try:
                        assert connection.recv(1) == b''
                    except (ssl.SSLError, ConnectionResetError):
                        pass
            # Whole-frame deadline closes a partial body.
            with tls_client(port) as connection:
                connection.sendall(struct.pack('!I', len(payload)) + payload[:1])
                start = time.monotonic()
                try:
                    assert connection.recv(1) == b''
                except (ssl.SSLError, ConnectionResetError):
                    pass
                assert time.monotonic() - start < TIMEOUT
            stalled.settimeout(TIMEOUT)
            assert stalled.recv(1) == b''
        # Clean exit runs worker joins and all destructors, also under sanitizers.
        output, errors = process.communicate(timeout=15)
        assert process.returncode == 0, (output, errors)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def client_case(certificate='server', ca=CA, name='localhost', success=True,
                version=None, malformed=None, blackhole=False):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(CERTS / (certificate + '.pem')), str(CERTS / (certificate + '-key.pem')))
    if version is not None:
        context.minimum_version = context.maximum_version = version
    errors = []
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(TIMEOUT)
        def peer():
            try:
                raw, _ = listener.accept()
                with raw:
                    raw.settimeout(TIMEOUT)
                    if blackhole:
                        time.sleep(2.2)
                        return
                    with context.wrap_socket(raw, server_side=True) as connection:
                        length, = struct.unpack('!I', exact(connection, 4))
                        data = exact(connection, length)
                        assert data == BINARY
                        connection.sendall(struct.pack('!I', len(data) if malformed is None else malformed))
                        if malformed is None:
                            # Split application writes to exercise partial record delivery.
                            for byte in data:
                                connection.sendall(bytes([byte]))
            except (ssl.SSLError, EOFError, ConnectionError):
                if success:
                    errors.append(sys.exc_info()[1])
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=peer)
        thread.start()
        try:
            completed = subprocess.run([EXE, 'client', str(ca), str(listener.getsockname()[1]),
                                        name, 'ok' if success else 'fail'],
                                       capture_output=True, text=True, timeout=TIMEOUT + 2)
            assert completed.returncode == 0, completed.stdout + completed.stderr
        finally:
            thread.join(TIMEOUT + 1)
        assert not thread.is_alive() and not errors, errors


for protocol in ('binary', 'compact'):
    server_cases(protocol)
    print('server', protocol, 'passed', flush=True)
for version in VERSIONS:
    client_case(version=version)
client_case(name='127.0.0.1')
client_case(name='wrong.invalid', success=False)
client_case(ca=CERTS / ('other-ca.der' if WINDOWS else 'other-ca.pem'), success=False)
client_case(certificate='expired', success=False)
client_case(malformed=FRAME_LIMIT + 1, success=False)
client_case(malformed=0, success=False)
client_case(blackhole=True, success=False)
print('client trust, hostname, expiry, framing, deadlines passed', flush=True)
