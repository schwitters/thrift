"""Regenerate public test-only identities; never use these keys in production."""
from datetime import datetime, timezone
from pathlib import Path
import ipaddress
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives.serialization import pkcs12
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

ROOT = Path(__file__).resolve().parent
START = datetime(2020, 1, 1, tzinfo=timezone.utc)
END = datetime(2040, 1, 1, tzinfo=timezone.utc)

def identity(name, issuer=None, expired=False):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    ca = issuer is None
    cert = (x509.CertificateBuilder().subject_name(subject)
            .issuer_name(subject if ca else issuer[1].subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(START)
            .not_valid_after(datetime(2021, 1, 1, tzinfo=timezone.utc) if expired else END)
            .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), critical=True))
    cert = cert.add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False)
    cert = cert.add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(
        key.public_key() if ca else issuer[0].public_key()), False)
    cert = cert.add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
        key_encipherment=not ca, data_encipherment=False, key_agreement=False,
        key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False), True)
    if not ca:
        cert = cert.add_extension(x509.SubjectAlternativeName([
            x509.DNSName('localhost'), x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), False)
        cert = cert.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False)
    cert = cert.sign(key if ca else issuer[0], hashes.SHA256())
    return key, cert

ca = identity('Thrift C11 TEST ONLY CA')
other = identity('Thrift C11 UNTRUSTED TEST ONLY CA')
for name, pair in [('ca', ca), ('other-ca', other)]:
    (ROOT / (name + '.pem')).write_bytes(pair[1].public_bytes(serialization.Encoding.PEM))
    (ROOT / (name + '.der')).write_bytes(pair[1].public_bytes(serialization.Encoding.DER))
for name, expired in [('server', False), ('expired', True)]:
    key, cert = identity('localhost', ca, expired)
    (ROOT / (name + '.pem')).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (ROOT / (name + '-key.pem')).write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    (ROOT / (name + '.pfx')).write_bytes(pkcs12.serialize_key_and_certificates(b'thrift-test', key,
        cert, [ca[1]], serialization.NoEncryption()))
