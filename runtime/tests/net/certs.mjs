// Test certificates for the networking rows, built with nothing but node:crypto.
//
// OpenSSL's command line differs between the LibreSSL macOS ships and OpenSSL
// 3 (neither sets a notAfter in the past the same way), so the certificates are
// assembled here as DER: a P-256 key from node:crypto, a TBSCertificate encoded
// by hand, an ECDSA-SHA256 signature from crypto.sign. Every one is generated
// fresh per server start, so nothing expires in the tree.
//
//   ca          the test CA the runtime is told to trust (RuntimeConfig.testTlsAnchors)
//   leaf        localhost + 127.0.0.1, signed by ca                 -> trusted
//   untrusted   localhost + 127.0.0.1, signed by an unrelated CA     -> untrusted
//   expired     localhost + 127.0.0.1, signed by ca, expired 30 days ago
//   wrongHost   wrong.example only, signed by ca                     -> wrong host

import { createHash, generateKeyPairSync, randomBytes, sign } from 'node:crypto';

function length(n) {
  if (n < 0x80) return Buffer.from([n]);
  const bytes = [];
  for (let v = n; v > 0; v >>= 8) bytes.unshift(v & 0xff);
  return Buffer.from([0x80 | bytes.length, ...bytes]);
}
const tlv = (tag, content) => Buffer.concat([Buffer.from([tag]), length(content.length), content]);
const seq = (...items) => tlv(0x30, Buffer.concat(items));
const set = (...items) => tlv(0x31, Buffer.concat(items));
const octets = (bytes) => tlv(0x04, bytes);
const bits = (bytes) => tlv(0x03, Buffer.concat([Buffer.from([0]), bytes]));
const bool = (value) => tlv(0x01, Buffer.from([value ? 0xff : 0x00]));
const explicit = (n, content) => tlv(0xa0 | n, content);

function oid(text) {
  const parts = text.split('.').map(Number);
  const out = [40 * parts[0] + parts[1]];
  for (const part of parts.slice(2)) {
    const stack = [part & 0x7f];
    for (let v = part >> 7; v > 0; v >>= 7) stack.unshift((v & 0x7f) | 0x80);
    out.push(...stack);
  }
  return tlv(0x06, Buffer.from(out));
}

// DER wants the minimal two's-complement form: no leading 0x00 unless the next
// byte's top bit needs it, and a 0x00 added when the first byte's is set. The
// serial is random, so a leading zero byte came up about once in 256
// certificates, and OpenSSL refuses the whole certificate ("illegal padding") --
// the fixture then failed to start, a few percent of the time.
function integer(bytes) {
  let start = 0;
  while (start < bytes.length - 1 && bytes[start] === 0 && !(bytes[start + 1] & 0x80)) start++;
  const trimmed = bytes.subarray(start);
  const b = trimmed[0] & 0x80 ? Buffer.concat([Buffer.from([0]), trimmed]) : trimmed;
  return tlv(0x02, b);
}

function time(date) {
  const p = (n) => String(n).padStart(2, '0');
  const text = `${p(date.getUTCFullYear() % 100)}${p(date.getUTCMonth() + 1)}${p(date.getUTCDate())}` +
               `${p(date.getUTCHours())}${p(date.getUTCMinutes())}${p(date.getUTCSeconds())}Z`;
  return tlv(0x17, Buffer.from(text, 'ascii'));
}

const name = (cn) => seq(set(seq(oid('2.5.4.3'), tlv(0x0c, Buffer.from(cn, 'utf8')))));
const extension = (id, critical, value) => seq(oid(id), ...(critical ? [bool(true)] : []), octets(value));
const ECDSA_SHA256 = seq(oid('1.2.840.10045.4.3.2'));
const DAY = 24 * 3600 * 1000;

function certificate({ subject, issuer, issuerKey, publicKey, ca, hosts, notBefore, notAfter }) {
  const spki = publicKey.export({ type: 'spki', format: 'der' });
  const keyId = createHash('sha1').update(spki).digest();
  const issuerId = createHash('sha1').update(issuer.spki).digest();
  const serial = randomBytes(8);
  serial[0] &= 0x7f;

  const extensions = [
    extension('2.5.29.19', true, ca ? seq(bool(true)) : seq()),
    // keyCertSign | cRLSign for the CA; digitalSignature for a server.
    extension('2.5.29.15', true, ca ? tlv(0x03, Buffer.from([1, 0x06])) : tlv(0x03, Buffer.from([7, 0x80]))),
    extension('2.5.29.14', false, octets(keyId)),
    extension('2.5.29.35', false, seq(tlv(0x80, issuerId))),
  ];
  if (!ca) {
    extensions.push(extension('2.5.29.37', false, seq(oid('1.3.6.1.5.5.7.3.1'))));
    const names = hosts.map((host) => /^\d+\.\d+\.\d+\.\d+$/.test(host)
      ? tlv(0x87, Buffer.from(host.split('.').map(Number)))
      : tlv(0x82, Buffer.from(host, 'ascii')));
    extensions.push(extension('2.5.29.17', false, seq(...names)));
  }

  const tbs = seq(
    explicit(0, integer(Buffer.from([2]))),
    integer(serial),
    ECDSA_SHA256,
    name(issuer.cn),
    seq(time(notBefore), time(notAfter)),
    name(subject),
    spki,
    explicit(3, seq(...extensions)),
  );
  const signature = sign('sha256', tbs, issuerKey);
  return seq(tbs, ECDSA_SHA256, bits(signature));
}

function pem(der) {
  const body = der.toString('base64').match(/.{1,64}/g).join('\n');
  return `-----BEGIN CERTIFICATE-----\n${body}\n-----END CERTIFICATE-----\n`;
}

function authority(cn, now) {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const self = { cn, spki: publicKey.export({ type: 'spki', format: 'der' }) };
  const der = certificate({
    subject: cn, issuer: self, issuerKey: privateKey, publicKey, ca: true,
    notBefore: new Date(now - DAY), notAfter: new Date(now + 365 * DAY),
  });
  return { ...self, key: privateKey, der, pem: pem(der) };
}

function server(ca, hosts, notBefore, notAfter) {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const der = certificate({
    subject: hosts[0], issuer: ca, issuerKey: ca.key, publicKey, ca: false, hosts, notBefore, notAfter,
  });
  return { cert: pem(der), key: privateKey.export({ type: 'pkcs8', format: 'pem' }) };
}

export function makeCertificates() {
  const now = Date.now();
  const ca = authority('ScreenKit Test CA', now);
  const stranger = authority('ScreenKit Untrusted CA', now);
  const valid = [new Date(now - DAY), new Date(now + 30 * DAY)];
  return {
    caDer: ca.der,
    caPem: ca.pem,
    leaf: server(ca, ['localhost', '127.0.0.1'], ...valid),
    untrusted: server(stranger, ['localhost', '127.0.0.1'], ...valid),
    expired: server(ca, ['localhost', '127.0.0.1'], new Date(now - 60 * DAY), new Date(now - 30 * DAY)),
    wrongHost: server(ca, ['wrong.example'], ...valid),
  };
}
