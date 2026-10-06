#!/usr/bin/env node
// The local fixture server the net-* ctest rows run against. Nothing in the
// suite reaches the public internet: every URL a row fetches is one of these.
//
//   node runtime/tests/net/server.mjs --dir <state-dir> [--ttl <seconds>]
//
// Listens on 127.0.0.1, ephemeral ports, and writes <dir>/servers.json once all
// of them are up:
//
//   http, https                 the fixture routes, plain and TLS (trusted cert)
//   httpsUntrusted, httpsExpired, httpsWrongHost
//                               the same routes behind a certificate the OS must refuse
//   closedPort                  a port nothing listens on (refused connections)
//   caDer                       the test CA, for RuntimeConfig.testTlsAnchors
//   media                       where the media fixtures were generated (media.mjs):
//                               served under /media/, with a fake licence server
//                               under /licence/
//
// Started and stopped by ctest (FIXTURES_SETUP / FIXTURES_CLEANUP); it also
// exits on its own after --ttl seconds (default 1800) so an interrupted run
// cannot leave it behind for long. See the route table in `handle` below.

import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import { join } from 'node:path';
import zlib from 'node:zlib';

import { makeCertificates } from './certs.mjs';
import { mediaRoute, prepareMedia } from './media.mjs';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const dir = opt('dir', null);
if (!dir) {
  console.error('usage: server.mjs --dir <state-dir> [--ttl <seconds>]');
  process.exit(64);
}
const ttl = Number(opt('ttl', '1800'));
mkdirSync(dir, { recursive: true });

// The media-* rows' streams, made with this machine's ffmpeg before anything
// listens (media.mjs). --no-media skips it, for a machine without ffmpeg that
// only runs the net rows.
const media = args.includes('--no-media') ? null : prepareMedia(dir);

// --- fixtures ------------------------------------------------------------------

const TEXT = 'The quick brown fox jumps over the lazy dog. '.repeat(40);

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function pngChunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(w, h, pixel) {
  const rows = [];
  for (let y = 0; y < h; y++) {
    rows.push(Buffer.from([0]));
    for (let x = 0; x < w; x++) rows.push(Buffer.from(pixel(x, y)));
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr), pngChunk('IDAT', zlib.deflateSync(Buffer.concat(rows))),
    pngChunk('IEND', Buffer.alloc(0))]);
}
// 13x7 and not uniform, so the image rows can tell a whole upload from a crop, a
// flip or a lost row: green, a red far corner (12,6), and one half-transparent
// texel (6,3) that shows whether the download path premultiplied it.
const PNG = png(13, 7, (x, y) => {
  if (x === 12 && y === 6) return [250, 10, 30, 255];
  if (x === 6 && y === 3) return [200, 100, 50, 128];
  return [12, 200, 80, 255];
});

// Per-id observations a row asks for afterwards through /stats: whether the
// client closed a connection early, how many chunks an upload had, what an
// EventSource reconnect carried.
const stats = new Map();
const statsFor = (id) => {
  if (!stats.has(id)) stats.set(id, { closed: false, finished: false, attempts: 0, lastEventIds: [], cookies: [] });
  return stats.get(id);
};

function json(res, status, value, headers = {}) {
  const body = Buffer.from(JSON.stringify(value));
  res.writeHead(status, { 'content-type': 'application/json', 'content-length': body.length, ...headers });
  res.end(body);
}

function readBody(req, done) {
  const chunks = [];
  let count = 0;
  req.on('data', (c) => { chunks.push(c); count++; });
  req.on('end', () => done(Buffer.concat(chunks), count));
}

// --- routes -----------------------------------------------------------------------

function handle(req, res) {
  const url = new URL(req.url, 'http://fixture');
  const q = url.searchParams;
  const path = url.pathname;
  res.setHeader('x-fixture', 'screenkit');

  if (media && mediaRoute(req, res, url, media.root)) return;

  if (path === '/data.json') return json(res, 200, { hello: 'world', n: 42 });

  if (path === '/echo') {
    return readBody(req, (body, chunks) => json(res, 200, {
      method: req.method, path: req.url, headers: req.headers, body: body.toString('base64'),
      length: body.length, chunks,
    }));
  }

  if (path.startsWith('/status/')) {
    const code = Number(path.slice(8));
    // A null-body status carries neither body nor Content-Length. It used to
    // carry both -- the row was about the client ending the response at its head
    // however the headers read -- but that is invalid HTTP (RFC 9110 8.6) and
    // OkHttp refuses it outright: `HTTP 204 had non-zero Content-Length`. What
    // the row is really for, that `response.body` is null and the connection is
    // still usable afterwards, survives without the malformed part.
    if (code === 204 || code === 205 || code === 304) {
      res.writeHead(code, { 'content-type': 'text/plain' });
      return res.end();
    }
    const body = Buffer.from(`status ${code} body`);
    res.writeHead(code, { 'content-type': 'text/plain', 'content-length': body.length });
    return res.end(body);
  }

  if (path === '/conn') {
    const body = Buffer.from(String(req.socket.remotePort));
    res.writeHead(200, { 'content-type': 'text/plain', 'content-length': body.length });
    return res.end(body);
  }

  // Redirects.
  if (path === '/redirect') {
    res.writeHead(Number(q.get('code') || 302), { location: q.get('to') || '/data.json', 'content-length': 0 });
    return res.end();
  }
  if (path.startsWith('/chain/')) {
    const n = Number(path.slice(7));
    if (n <= 0) return json(res, 200, { hello: 'world', n: 42, chain: 'end' });
    res.writeHead(302, { location: `/chain/${n - 1}`, 'content-length': 0 });
    return res.end();
  }
  if (path === '/loop') {
    res.writeHead(302, { location: '/loop', 'content-length': 0 });
    return res.end();
  }

  // Encodings.
  if (path === '/chunked') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    const parts = ['one-', 'two-', 'three'];
    let i = 0;
    const next = () => {
      if (i === parts.length) return res.end();
      res.write(parts[i++]);
      setTimeout(next, 10);
    };
    return next();
  }
  if (path === '/gzip' || path === '/deflate' || path === '/deflate-raw') {
    const body = path === '/gzip' ? zlib.gzipSync(TEXT) : path === '/deflate' ? zlib.deflateSync(TEXT) : zlib.deflateRawSync(TEXT);
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': path === '/gzip' ? 'gzip' : 'deflate',
                         'content-length': body.length });
    return res.end(body);
  }
  if (path === '/gzip-chunked') {
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip' });
    const body = zlib.gzipSync(TEXT);
    res.write(body.subarray(0, 100));
    return setTimeout(() => res.end(body.subarray(100)), 20);
  }
  if (path === '/gzip-truncated') {
    const body = zlib.gzipSync(TEXT);
    // Content-Length says all of it; the connection ends at half.
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip', 'content-length': body.length });
    res.write(body.subarray(0, body.length >> 1));
    return setTimeout(() => res.socket.destroy(), 20);
  }
  if (path === '/gzip-empty') {
    // An empty body is a complete empty body, compressed or not.
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip', 'content-length': 0 });
    return res.end();
  }
  if (path === '/gzip-empty-chunked') {
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip', 'transfer-encoding': 'chunked' });
    return res.end();
  }
  if (path === '/conflicting-length') {
    // Two Content-Length lines that disagree: written raw, since Node would
    // refuse to send them.
    return req.socket.end('HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n' +
                          'Content-Length: 6\r\n\r\nhello!');
  }
  if (path === '/gzip-cut') {
    // A gzip stream that stops early but whose framing (chunked) ends cleanly.
    const body = zlib.gzipSync(TEXT);
    res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip' });
    return res.end(body.subarray(0, body.length >> 1));
  }
  if (path === '/chunked-truncated') {
    res.writeHead(200, { 'content-type': 'text/plain' });
    res.write('first chunk');
    return setTimeout(() => res.socket.destroy(), 20);
  }
  if (path === '/length-truncated') {
    res.writeHead(200, { 'content-type': 'text/plain', 'content-length': 100 });
    res.write('only ten b');
    return setTimeout(() => res.socket.destroy(), 20);
  }

  // Streaming and hanging.
  if (path === '/slow') {
    const id = q.get('id') || 'slow';
    const chunks = Number(q.get('chunks') || 3);
    const interval = Number(q.get('interval') || 200);
    const s = statsFor(id);
    // `type` exists for `text/plain`, which CFNetwork's content sniffing holds
    // back until 512 bytes have arrived unless sniffing is turned off
    // (NetServiceApple.mm); net-streaming-response asserts that it is.
    res.writeHead(200, { 'content-type': q.get('type') || 'application/octet-stream' });
    let i = 0;
    let timer = null;
    res.on('close', () => { if (!s.finished) s.closed = true; clearTimeout(timer); });
    const next = () => {
      if (i === chunks) { s.finished = true; return res.end(); }
      res.write(`chunk-${i++};`);
      timer = setTimeout(next, interval);
    };
    return next();
  }
  // Flow control: a large body written as fast as the client takes it. `written`
  // in /stats is how much the server got onto the wire, which stalls when the
  // client stops reading.
  if (path === '/firehose') {
    const s = statsFor(q.get('id') || 'firehose');
    const total = Number(q.get('bytes') || 0);
    // `rate`, in bytes per 10 ms, paces the writes. Unpaced -- the default --
    // measures what the client does when the server can outrun it, which on
    // Apple is the recorded best-effort case; paced slowly enough for any build
    // to keep up, it measures the flow window itself, which both clients honour.
    const rate = Number(q.get('rate') || 0);
    const chunk = Buffer.alloc(64 * 1024, 0x61);
    s.written = 0;
    res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': total });
    res.on('close', () => { if (!s.finished) s.closed = true; });
    // Backpressure is honoured either way -- `written` is what the connection
    // took, not what Node buffered -- and `rate` only caps how much of it is
    // offered per tick. A drain resumes the *same* tick's budget; refilling it
    // there would make the pacing vanish the moment the client read quickly.
    let budget = rate > 0 ? rate : Infinity;
    const pump = () => {
      while (s.written < total && budget > 0) {
        const n = Math.min(chunk.length, total - s.written, budget);
        s.written += n;
        budget -= n;
        if (!res.write(n === chunk.length ? chunk : chunk.subarray(0, n))) {
          res.once('drain', pump);
          return;
        }
      }
      if (s.written < total) {
        return void setTimeout(() => { budget = rate; pump(); }, 10);
      }
      s.finished = true;
      res.end();
    };
    return pump();
  }
  // The per-origin connection limit: how many of these run at once.
  if (path === '/slot') {
    const s = statsFor(q.get('id') || 'slot');
    s.active = (s.active || 0) + 1;
    s.maxActive = Math.max(s.maxActive || 0, s.active);
    setTimeout(() => {
      s.active -= 1;
      s.served = (s.served || 0) + 1;
      json(res, 200, { ok: true });
    }, Number(q.get('ms') || 200));
    return;
  }
  if (path === '/hang') {
    const s = statsFor(q.get('id') || 'hang');
    req.on('close', () => { s.closed = true; });
    res.on('close', () => { s.closed = true; });
    return;  // never answers
  }
  if (path === '/upload') {
    const s = statsFor(q.get('id') || 'upload');
    let ended = false;
    const chunks = [];
    req.on('data', (c) => { chunks.push(c); s.chunks = (s.chunks || 0) + 1; });
    req.on('end', () => {
      ended = true;
      s.finished = true;
      json(res, 200, { body: Buffer.concat(chunks).toString(), chunked: req.headers['transfer-encoding'] || '' });
    });
    req.on('close', () => { if (!ended) s.closed = true; });
    return;
  }
  // A server that takes the connection and never reads the body: the socket's
  // receive window fills, the client's writes stop going anywhere, and whatever
  // the page keeps writing piles up on the client side. That is the only way to
  // reach a client's own upload cap from a test (net-upload-overflow).
  if (path === '/sink-stalled') {
    const s = statsFor(q.get('id') || 'sink-stalled');
    // `pause()` and no listener: node stops reading the request body off the
    // socket, so the window closes rather than the bytes being buffered here.
    req.pause();
    // Both, as /hang does: a paused request is not being read, so node notices
    // the peer going away on the *response* side first.
    req.on('close', () => { s.closed = true; });
    res.on('close', () => { s.closed = true; });
    return;  // never reads, never answers
  }
  if (path === '/stats') return json(res, 200, statsFor(q.get('id') || ''));

  // Cookies.
  if (path === '/set-cookie') {
    res.writeHead(200, { 'set-cookie': q.getAll('c'), 'content-type': 'text/plain', 'content-length': 2 });
    return res.end('ok');
  }
  if (path === '/cookies' || path.startsWith('/cookies/')) return json(res, 200, { cookie: req.headers.cookie || '' });

  // EventSource.
  if (path === '/events') {
    const s = statsFor(q.get('id') || 'events');
    s.attempts++;
    s.lastEventIds.push(req.headers['last-event-id'] === undefined ? null : req.headers['last-event-id']);
    s.cookies.push(req.headers.cookie || '');
    res.writeHead(200, { 'content-type': 'text/event-stream; charset=utf-8', 'cache-control': 'no-cache' });
    if (s.attempts === 1) {
      res.write('retry: 150\n\n');
      res.write(': a comment\n\n');
      res.write('data: first\n\n');
      res.write('event: custom\ndata: named\nid: 7\n\n');
      res.write('data: line1\r\ndata: line2\r\n\r\n');
      // Dropped without an end: the client reconnects after `retry`.
      return setTimeout(() => res.socket.destroy(), 100);
    }
    res.write(`data: again ${req.headers['last-event-id'] || ''}\n\n`);
    res.on('close', () => { s.closed = true; });
    return;  // held open until the client closes it
  }
  if (path === '/events-404') {
    statsFor(q.get('id') || 'events-404').attempts++;
    res.writeHead(404, { 'content-type': 'text/event-stream', 'content-length': 0 });
    return res.end();
  }
  if (path === '/events-wrong-type') {
    statsFor(q.get('id') || 'events-wrong-type').attempts++;
    res.writeHead(200, { 'content-type': 'text/plain', 'content-length': 12 });
    return res.end('data: nope\n\n');
  }

  // Images.
  if (path === '/image.png') {
    res.writeHead(200, { 'content-type': 'image/png', 'content-length': PNG.length });
    return res.end(PNG);
  }
  if (path === '/not-image.png') {
    const body = Buffer.from('this is not a png at all');
    res.writeHead(200, { 'content-type': 'image/png', 'content-length': body.length });
    return res.end(body);
  }

  json(res, 404, { error: 'no route', path });
}

// --- WebSocket ----------------------------------------------------------------------

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function frame(opcode, payload, fin = true) {
  const len = payload.length;
  const first = (fin ? 0x80 : 0) | opcode;
  let head;
  if (len < 126) head = Buffer.from([first, len]);
  else if (len < 65536) { head = Buffer.alloc(4); head[0] = first; head[1] = 126; head.writeUInt16BE(len, 2); }
  else { head = Buffer.alloc(10); head[0] = first; head[1] = 127; head.writeBigUInt64BE(BigInt(len), 2); }
  return Buffer.concat([head, payload]);
}

function upgrade(req, socket, head) {
  const url = new URL(req.url, 'http://fixture');
  if (url.pathname === '/ws-refuse') {
    socket.end('HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n');
    return;
  }
  // Handshakes a client must refuse: a wrong accept key, a subprotocol it did
  // not ask for, an extension it did not offer (`/ws-unknown-extension`, which
  // no client offers). `/ws-extension` answers `permessage-deflate`, which
  // NSURLSession and OkHttp both offer and the Linux client does not -- so it
  // is a negotiation on two clients and a refusal on the third. It never sends
  // a compressed frame, which permessage-deflate allows.
  const broken = { '/ws-bad-accept': 1, '/ws-bad-protocol': 1, '/ws-extension': 1, '/ws-unknown-extension': 1 };
  if (url.pathname !== '/ws' && !broken[url.pathname]) {
    socket.destroy();
    return;
  }
  const key = req.headers['sec-websocket-key'];
  const accept = createHash('sha1').update((url.pathname === '/ws-bad-accept' ? 'x' : '') + key + GUID).digest('base64');
  const offered = (req.headers['sec-websocket-protocol'] || '').split(',').map((s) => s.trim()).filter(Boolean);
  const chosen = url.pathname === '/ws-bad-protocol' ? 'unrequested' : offered.length ? offered[offered.length - 1] : null;
  const cookie = req.headers.cookie || '';
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               `Sec-WebSocket-Accept: ${accept}\r\n` +
               (chosen ? `Sec-WebSocket-Protocol: ${chosen}\r\n` : '') +
               (url.pathname === '/ws-extension' ? 'Sec-WebSocket-Extensions: permessage-deflate\r\n' : '') +
               (url.pathname === '/ws-unknown-extension' ? 'Sec-WebSocket-Extensions: x-screenkit-unoffered\r\n' : '') + '\r\n');

  let buffer = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
  let closing = false;
  let pendingPing = null;
  const send = (opcode, payload) => { if (!socket.destroyed) socket.write(frame(opcode, payload)); };

  const onMessage = (text, payload) => {
    if (text) {
      const message = payload.toString('utf8');
      if (message.startsWith('close:')) {
        const [, code, reason] = message.split(':');
        const body = Buffer.alloc(2 + Buffer.byteLength(reason || ''));
        body.writeUInt16BE(Number(code), 0);
        body.write(reason || '', 2);
        closing = true;
        send(0x8, body);
        return;
      }
      if (message.startsWith('ping:')) {
        pendingPing = message.slice(5);
        send(0x9, Buffer.from(pendingPing));
        return;
      }
      if (message === 'cookie') return send(0x1, Buffer.from(`cookie:${cookie}`));
      if (message === 'pair') {
        // Two messages in one write, so they arrive together.
        if (!socket.destroyed) socket.write(Buffer.concat([frame(0x1, Buffer.from('first')), frame(0x1, Buffer.from('second'))]));
        return;
      }
      if (message === 'fragments') {
        // A text message in three fragments with a ping between them, then a
        // binary message in two: each must arrive once, reassembled, typed.
        if (!socket.destroyed) {
          socket.write(Buffer.concat([
            frame(0x1, Buffer.from('frag'), false),
            frame(0x9, Buffer.from('between')),
            frame(0x0, Buffer.from('men'), false),
            frame(0x0, Buffer.from('ted'), true),
            frame(0x2, Buffer.from([1, 2]), false),
            frame(0x0, Buffer.from([3]), true),
            frame(0x1, Buffer.from('fragments-done')),
          ]));
        }
        return;
      }
      if (message.startsWith('burst:')) {
        // Many small messages at once: more than SDL's event queue holds.
        const count = Number(message.slice(6));
        const frames = [];
        for (let i = 0; i < count; i++) frames.push(frame(0x1, Buffer.from(String(i))));
        frames.push(frame(0x1, Buffer.from('burst-done')));
        if (!socket.destroyed) socket.write(Buffer.concat(frames));
        return;
      }
      if (message === 'drop') return socket.destroy();
      return send(0x1, payload);
    }
    send(0x2, payload);
  };

  socket.on('data', (data) => {
    buffer = Buffer.concat([buffer, data]);
    for (;;) {
      if (buffer.length < 2) return;
      const opcode = buffer[0] & 0x0f;
      let len = buffer[1] & 0x7f;
      let offset = 2;
      if (len === 126) { if (buffer.length < 4) return; len = buffer.readUInt16BE(2); offset = 4; }
      else if (len === 127) { if (buffer.length < 10) return; len = Number(buffer.readBigUInt64BE(2)); offset = 10; }
      const masked = (buffer[1] & 0x80) !== 0;
      if (!masked) { socket.destroy(); return; }  // RFC 6455: a client must mask
      if (buffer.length < offset + 4 + len) return;
      const mask = buffer.subarray(offset, offset + 4);
      const payload = Buffer.from(buffer.subarray(offset + 4, offset + 4 + len));
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      buffer = buffer.subarray(offset + 4 + len);

      if (opcode === 0x1 || opcode === 0x2) onMessage(opcode === 0x1, payload);
      else if (opcode === 0x8) {
        // Echo the whole close payload, reason included, so a row can see the
        // client's reason arrived.
        if (!closing) send(0x8, payload);
        socket.end();
        return;
      } else if (opcode === 0x9) send(0xA, payload);
      else if (opcode === 0xA && pendingPing !== null) {
        send(0x1, Buffer.from(`pong:${payload.toString('utf8')}`));
        pendingPing = null;
      }
    }
  });
  socket.on('error', () => {});
}

// --- servers ------------------------------------------------------------------------

const certs = makeCertificates();
writeFileSync(join(dir, 'ca.der'), certs.caDer);
writeFileSync(join(dir, 'ca.pem'), certs.caPem);

function listen(server) {
  server.on('upgrade', upgrade);
  server.on('clientError', (err, socket) => socket.destroy());
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
}

// A throw in a route is that request's 500, not the end of the fixture: this
// process serves every row of a run, and one bad path used to take the rest of
// them down with it.
function serve(req, res) {
  try {
    handle(req, res);
  } catch (e) {
    console.error(`fixture: ${req.method} ${req.url} threw`, e);
    if (!res.headersSent) res.writeHead(500, { 'content-type': 'text/plain' });
    res.end('fixture error');
  }
}

const plain = http.createServer(serve);
const secure = https.createServer({ ...certs.leaf }, serve);
const untrusted = https.createServer({ ...certs.untrusted }, serve);
const expired = https.createServer({ ...certs.expired }, serve);
const wrongHost = https.createServer({ ...certs.wrongHost }, serve);
for (const s of [secure, untrusted, expired, wrongHost]) s.on('tlsClientError', () => {});

const ports = {
  http: await listen(plain),
  https: await listen(secure),
  httpsUntrusted: await listen(untrusted),
  httpsExpired: await listen(expired),
  httpsWrongHost: await listen(wrongHost),
};
// A port nothing listens on: probed after every fixture port is bound, and never
// one of them.
const taken = new Set(Object.values(ports));
let closed = 0;
while (closed === 0 || taken.has(closed)) {
  closed = await new Promise((resolve) => {
    const probe = net.createServer();
    probe.listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}
ports.closedPort = closed;
const description = {
  ...ports,
  caDer: join(dir, 'ca.der'),
  media: media ? media.root : null,
  pid: process.pid,
};
writeFileSync(join(dir, 'server.pid'), String(process.pid));
writeFileSync(join(dir, 'servers.json.tmp'), JSON.stringify(description, null, 2));
// Renamed into place, so a reader never sees half a file.
import('node:fs').then(({ renameSync }) => renameSync(join(dir, 'servers.json.tmp'), join(dir, 'servers.json')));
console.log(`fixture servers up: ${JSON.stringify(description)}`);

const shutdown = () => {
  rmSync(join(dir, 'servers.json'), { force: true });
  process.exit(0);
};
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
setTimeout(shutdown, ttl * 1000).unref();
