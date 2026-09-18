# Local TLS test certificate

`localhost.crt` and `localhost.key` are public, test-only credentials for the
loopback Python WSS peer. They are not deployment credentials. The certificate
covers `localhost` and `127.0.0.1`, is self-signed, and is passed explicitly as
`ca_file` only for the positive TLS test. The negative test omits that CA and
requires verification failure.

Generated with OpenSSL for ten years from September 2026. Renew the fixture
before expiration with:

```sh
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout localhost.key -out localhost.crt -days 3650 \
  -subj /CN=localhost -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
```
