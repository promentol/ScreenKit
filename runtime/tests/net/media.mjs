// The media the media-* ctest rows play, generated when the fixture server
// starts, with this machine's own `ffmpeg` -- nothing is committed and nothing
// is fetched. Everything is small (seconds of 160x90 to 640x360 test pattern
// and a sine tone), so generation takes a few seconds.
//
//   <dir>/media/
//     vod/master.m3u8          two-variant HLS VOD (320x180, 640x360; H.264 + AAC,
//                              2 s TS segments, 10 s) with a WebVTT subtitle
//                              rendition whose cues change every 2 s
//     live/live.m3u8           a live sliding window over 2 s segments, served
//                              dynamically: the media sequence advances with the clock
//     dash/manifest.mpd        DASH, two video representations and one audio
//     mp4/progressive.mp4      progressive MP4, faststart
//     big/progressive.mp4      the same at 1280x720, for the stats rows
//     cenc/clearkey.mpd        CENC (cenc-aes-ctr) DASH, ContentProtection for ClearKey
//     cenc/widevine.mpd        the same segments, ContentProtection for Widevine with a pssh
//     fairplay/master.m3u8     HLS naming a FairPlay key (skd://), for the key-request path
//     corrupt/corrupt.mp4      an 'ftyp' box followed by garbage
//
// and the routes that serve it (`mediaRoute`), statically with Range, plus a
// fake licence server that records what reached it (`/licence/...`).
//
// FFMPEG=<path> picks the binary; otherwise the first `ffmpeg` on PATH.

import { execFileSync } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { extname, join, normalize, sep } from 'node:path';

// The ClearKey key the cenc rows configure with drm.clearKeys, as Shaka takes
// them: key id -> key, hex.
export const CLEARKEY_KID = 'a7e61c373e219033c21091fa607bf3b8';
export const CLEARKEY_KEY = '76a6c65c5ea762046bd749a2e632ccbb';
// The system ids ContentProtection names.
const CLEARKEY_SYSTEM = 'e2719d58-a985-b3c9-781a-b030af78d30e';
const WIDEVINE_SYSTEM = 'edef8ba9-79d6-4ace-a3c8-27dcd51d21ed';

const LIVE_SEGMENTS = 60;       // 2 minutes of material, looped with a discontinuity
// Segments in the playlist at once. Six, not three: AVPlayer holds back three
// target durations from the live edge, so a three-segment window would leave it
// a seekable range of zero length.
const LIVE_WINDOW = 6;
const SEGMENT_SECONDS = 2;

function ffmpegBinary() {
  if (process.env.FFMPEG) return process.env.FFMPEG;
  return 'ffmpeg';
}

function run(args, cwd) {
  execFileSync(ffmpegBinary(), ['-hide_banner', '-loglevel', 'error', '-y', ...args], { cwd, stdio: ['ignore', 'ignore', 'pipe'] });
}

// A test pattern and a tone, `seconds` long.
function sources(seconds, size, rate = 30) {
  return ['-f', 'lavfi', '-i', `testsrc2=size=${size}:rate=${rate}:duration=${seconds}`,
          '-f', 'lavfi', '-i', `sine=frequency=440:sample_rate=48000:duration=${seconds}`];
}

// Closed GOPs on the segment boundary, so every segment starts with a keyframe.
function h264(bitrate, level, rate = 30) {
  return ['-c:v', 'libx264', '-preset', 'veryfast', '-profile:v', 'main', '-level:v', level,
          '-pix_fmt', 'yuv420p', '-g', String(rate * SEGMENT_SECONDS), '-keyint_min', String(rate * SEGMENT_SECONDS),
          '-sc_threshold', '0', '-b:v', bitrate, '-maxrate', bitrate, '-bufsize', bitrate];
}
const AAC = ['-c:a', 'aac', '-b:a', '64k', '-ac', '2', '-ar', '48000'];

function vttTime(seconds) {
  const ms = Math.round(seconds * 1000);
  const h = String(Math.floor(ms / 3600000)).padStart(2, '0');
  const m = String(Math.floor(ms / 60000) % 60).padStart(2, '0');
  const s = String(Math.floor(ms / 1000) % 60).padStart(2, '0');
  return `${h}:${m}:${s}.${String(ms % 1000).padStart(3, '0')}`;
}

// The cues every row can predict: "Cue 1" over [0, 2), "Cue 2" over [2, 4), ...
export const VOD_SECONDS = 10;
export const HARDWARE_SECONDS = 12;
export function vodCueText(index) { return `Cue ${index + 1}`; }

function makeVod(root) {
  const dir = join(root, 'vod');
  mkdirSync(join(dir, 'low'), { recursive: true });
  mkdirSync(join(dir, 'high'), { recursive: true });
  const renditions = [
    { name: 'low', size: '320x180', bitrate: '300k', level: '2.1', bandwidth: 450000, codecs: 'avc1.4d4015,mp4a.40.2' },
    { name: 'high', size: '640x360', bitrate: '800k', level: '3.0', bandwidth: 1000000, codecs: 'avc1.4d401e,mp4a.40.2' },
  ];
  for (const r of renditions) {
    run([...sources(VOD_SECONDS, r.size), ...h264(r.bitrate, r.level), ...AAC,
         '-f', 'hls', '-hls_time', String(SEGMENT_SECONDS), '-hls_playlist_type', 'vod',
         '-hls_segment_filename', join(dir, r.name, 'seg%03d.ts'), join(dir, r.name, 'index.m3u8')]);
  }
  // The subtitle rendition: one WebVTT segment over the whole stream. The
  // timestamp map ties cue time 0 to the TS segments' first PTS, which ffmpeg
  // starts at 1.4 s (126000 at 90 kHz), so a cue's time is presentation time.
  const cues = [];
  for (let i = 0; i * SEGMENT_SECONDS < VOD_SECONDS; i++) {
    cues.push(`${vttTime(i * SEGMENT_SECONDS)} --> ${vttTime((i + 1) * SEGMENT_SECONDS)}\n${vodCueText(i)}\n`);
  }
  writeFileSync(join(dir, 'subs.vtt'), `WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:126000,LOCAL:00:00:00.000\n\n${cues.join('\n')}`);
  writeFileSync(join(dir, 'subs.m3u8'),
    '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:' + VOD_SECONDS + '\n#EXT-X-MEDIA-SEQUENCE:0\n' +
    '#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:' + VOD_SECONDS.toFixed(3) + ',\nsubs.vtt\n#EXT-X-ENDLIST\n');
  const master = ['#EXTM3U', '#EXT-X-VERSION:3',
    '#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs",NAME="English",LANGUAGE="en",DEFAULT=NO,AUTOSELECT=YES,URI="subs.m3u8"'];
  for (const r of renditions) {
    master.push(`#EXT-X-STREAM-INF:BANDWIDTH=${r.bandwidth},AVERAGE-BANDWIDTH=${r.bandwidth},` +
                `RESOLUTION=${r.size},FRAME-RATE=30.000,CODECS="${r.codecs}",SUBTITLES="subs"`);
    master.push(`${r.name}/index.m3u8`);
  }
  writeFileSync(join(dir, 'master.m3u8'), master.join('\n') + '\n');
  // The same stream without the subtitle group, for a row that wants none.
  writeFileSync(join(dir, 'nosubs.m3u8'), master.filter((l) => !l.startsWith('#EXT-X-MEDIA'))
    .map((l) => l.replace(',SUBTITLES="subs"', '')).join('\n') + '\n');
}

function makeLive(root) {
  const dir = join(root, 'live');
  mkdirSync(dir, { recursive: true });
  run([...sources(LIVE_SEGMENTS * SEGMENT_SECONDS, '320x180'), ...h264('250k', '2.1'), ...AAC,
       '-f', 'hls', '-hls_time', String(SEGMENT_SECONDS), '-hls_playlist_type', 'vod',
       '-hls_segment_filename', join(dir, 'seg%03d.ts'), join(dir, 'all.m3u8')]);
}

function makeDash(root) {
  const dir = join(root, 'dash');
  mkdirSync(dir, { recursive: true });
  run([...sources(VOD_SECONDS, '640x360'),
       '-map', '0:v', '-map', '0:v', '-map', '1:a',
       '-c:v', 'libx264', '-preset', 'veryfast', '-profile:v', 'main', '-pix_fmt', 'yuv420p',
       '-g', String(30 * SEGMENT_SECONDS), '-keyint_min', String(30 * SEGMENT_SECONDS), '-sc_threshold', '0',
       '-s:v:0', '320x180', '-b:v:0', '300k', '-s:v:1', '640x360', '-b:v:1', '800k', ...AAC,
       '-f', 'dash', '-seg_duration', String(SEGMENT_SECONDS), '-use_template', '1', '-use_timeline', '0',
       '-adaptation_sets', 'id=0,streams=v id=1,streams=a', '-init_seg_name', 'init-$RepresentationID$.m4s',
       '-media_seg_name', 'chunk-$RepresentationID$-$Number%05d$.m4s', join(dir, 'manifest.mpd')]);
}

function makeMp4(root, name, size, bitrate, level) {
  const dir = join(root, name);
  mkdirSync(dir, { recursive: true });
  run([...sources(VOD_SECONDS, size), ...h264(bitrate, level), ...AAC, '-movflags', '+faststart',
       join(dir, 'progressive.mp4')]);
}

// 1080p30, for the one measurement a small clip cannot make: that a device's
// hardware decoder keeps up with the picture size its probe claims (the Pi 3's
// bcm2835-codec, through the ScreenKit VLC plugin -- media-hardware). Noise on
// the test pattern so the encoder produces real bitrate rather than a few
// kilobits of flat colour, which any decoder can keep up with.
function makeHardware(root) {
  const dir = join(root, 'hw');
  mkdirSync(dir, { recursive: true });
  run(['-f', 'lavfi', '-i', `testsrc2=size=1920x1080:rate=30:duration=${HARDWARE_SECONDS}`,
       '-f', 'lavfi', '-i', `sine=frequency=440:sample_rate=48000:duration=${HARDWARE_SECONDS}`,
       '-filter:v', 'noise=alls=12:allf=t', ...h264('6000k', '4.0'), ...AAC, '-movflags', '+faststart',
       join(dir, '1080p30.mp4')]);
}

// A Widevine pssh box's data: the WidevinePsshData protobuf with one key id
// (field 2) and a content id (field 4). Enough for a CDM to build a licence
// request; no licence server will ever answer it with a real licence.
function widevinePsshData() {
  const kid = Buffer.from(CLEARKEY_KID, 'hex');
  const content = Buffer.from('screenkit-fixture');
  return Buffer.concat([Buffer.from([0x12, kid.length]), kid, Buffer.from([0x22, content.length]), content]);
}

function psshBox(systemId, data) {
  const system = Buffer.from(systemId.replace(/-/g, ''), 'hex');
  const size = 8 + 4 + 16 + 4 + data.length;
  const head = Buffer.alloc(12);
  head.writeUInt32BE(size, 0);
  head.write('pssh', 4, 'ascii');
  head.writeUInt32BE(0, 8);  // version 0, flags 0
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  return Buffer.concat([head, system, length, data]);
}

// FFmpeg's CENC writer (movenc, which the dash muxer uses) carries each earlier
// fragment's sample auxiliary information forward: fragment k's senc and saiz
// hold the entries of fragments 1..k, and saio points at the first of them -- so
// every fragment would be decrypted with fragment 1's IVs, and a strict player
// refuses the segment outright (ExoPlayer: "Saiz sample count 120 is greater
// than fragment sample count 60"). Measured with FFmpeg 8.0, in both the dash
// muxer and a fragmented mp4. This keeps each fragment's own entries -- the last
// `trun` count of them, where saio already points once the stale ones are gone
// -- and shrinks every size and offset that spans what was removed: senc, saiz,
// traf, moof, the trun's data offset and the sidx reference.
function repairCencSegment(buf) {
  const box = (at) => ({ at, size: buf.readUInt32BE(at), type: buf.toString('latin1', at + 4, at + 8) });
  const children = (start, end) => {
    const out = [];
    for (let at = start; at + 8 <= end;) {
      const b = box(at);
      if (b.size < 8) break;
      out.push(b);
      at += b.size;
    }
    return out;
  };
  const top = children(0, buf.length);
  const sidx = top.find((b) => b.type === 'sidx');
  const moof = top.find((b) => b.type === 'moof');
  if (!moof) return buf;
  const trafs = children(moof.at + 8, moof.at + moof.size).filter((b) => b.type === 'traf');
  if (trafs.length !== 1) return buf;
  const traf = trafs[0];
  const kids = children(traf.at + 8, traf.at + traf.size);
  const trun = kids.find((b) => b.type === 'trun');
  const senc = kids.find((b) => b.type === 'senc');
  const saiz = kids.find((b) => b.type === 'saiz');
  if (!trun || !senc || !saiz || senc.at < trun.at) return buf;
  const samples = buf.readUInt32BE(trun.at + 12);
  const sencFlags = buf.readUInt32BE(senc.at + 8) & 0xffffff;
  const entries = buf.readUInt32BE(senc.at + 12);
  if (entries <= samples) return buf;
  const drop = entries - samples;
  // senc entries: an 8-byte IV (tenc's per-sample IV size), then subsamples.
  let p = senc.at + 16;
  for (let i = 0; i < drop; i++) {
    p += 8;
    if (sencFlags & 2) p += 2 + 6 * buf.readUInt16BE(p);
  }
  const sencStart = senc.at + 16, sencDropped = p - sencStart;
  let q = saiz.at + 12;
  if (buf.readUInt32BE(saiz.at + 8) & 1) q += 8;
  const defaultSize = buf[q];
  const saizTable = q + 5, saizDropped = defaultSize === 0 ? drop : 0;
  const removed = sencDropped + saizDropped;
  // Every field spanning the removed bytes is in a header ahead of them.
  buf = Buffer.from(buf);
  buf.writeUInt32BE(senc.size - sencDropped, senc.at);
  buf.writeUInt32BE(samples, senc.at + 12);
  buf.writeUInt32BE(saiz.size - saizDropped, saiz.at);
  buf.writeUInt32BE(samples, q + 1);
  buf.writeUInt32BE(traf.size - removed, traf.at);
  buf.writeUInt32BE(moof.size - removed, moof.at);
  if (buf.readUInt32BE(trun.at + 8) & 1) buf.writeInt32BE(buf.readInt32BE(trun.at + 16) - removed, trun.at + 16);
  if (sidx) {
    const version = buf[sidx.at + 8];
    const refs = sidx.at + 8 + 4 + 8 + (version === 0 ? 8 : 16) + 4;
    const reference = buf.readUInt32BE(refs);
    buf.writeUInt32BE((reference & 0x80000000) | ((reference & 0x7fffffff) - removed), refs);
  }
  return Buffer.concat([buf.subarray(0, sencStart), buf.subarray(sencStart + sencDropped, saizTable),
                        buf.subarray(saizTable + saizDropped)]);
}

function makeCenc(root) {
  const dir = join(root, 'cenc');
  mkdirSync(dir, { recursive: true });
  run([...sources(VOD_SECONDS, '320x180'), '-map', '0:v', '-map', '1:a',
       ...h264('300k', '2.1'), ...AAC,
       '-f', 'dash', '-seg_duration', String(SEGMENT_SECONDS), '-use_template', '1', '-use_timeline', '0',
       '-adaptation_sets', 'id=0,streams=v id=1,streams=a', '-init_seg_name', 'init-$RepresentationID$.m4s',
       '-media_seg_name', 'chunk-$RepresentationID$-$Number%05d$.m4s',
       '-format_options', `encryption_scheme=cenc-aes-ctr:encryption_key=${CLEARKEY_KEY}:encryption_kid=${CLEARKEY_KID}`,
       join(dir, 'plain.mpd')]);
  for (const name of readdirSync(dir)) {
    if (/^chunk-.*\.m4s$/.test(name)) writeFileSync(join(dir, name), repairCencSegment(readFileSync(join(dir, name))));
  }
  const mpd = readFileSync(join(dir, 'plain.mpd'), 'utf8');
  const kid = CLEARKEY_KID.replace(/^(.{8})(.{4})(.{4})(.{4})(.{12})$/, '$1-$2-$3-$4-$5');
  const mp4protection = `<ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc" cenc:default_KID="${kid}"/>`;
  const withProtection = (system, pssh) => mpd
    .replace('<MPD ', '<MPD xmlns:cenc="urn:mpeg:cenc:2013" ')
    .replace(/(<AdaptationSet[^>]*>)/g, `$1\n\t\t\t${mp4protection}\n\t\t\t<ContentProtection schemeIdUri="urn:uuid:${system}"` +
             (pssh ? `>\n\t\t\t\t<cenc:pssh>${pssh.toString('base64')}</cenc:pssh>\n\t\t\t</ContentProtection>` : '/>'));
  writeFileSync(join(dir, 'clearkey.mpd'), withProtection(CLEARKEY_SYSTEM, null));
  writeFileSync(join(dir, 'widevine.mpd'), withProtection(WIDEVINE_SYSTEM, psshBox(WIDEVINE_SYSTEM, widevinePsshData())));
}

// HLS that names a FairPlay key. The segments are the VOD's low rendition and
// are not encrypted: what this exercises is AVFoundation asking for the key --
// certificate fetch, the SPC request, the licence round trip -- which starts
// from the playlist, before any sample is decrypted.
export const FAIRPLAY_CONTENT_ID = 'skd://screenkit-fixture-asset';
function makeFairPlay(root) {
  const dir = join(root, 'fairplay');
  mkdirSync(dir, { recursive: true });
  const low = readFileSync(join(root, 'vod', 'low', 'index.m3u8'), 'utf8').split('\n');
  const out = [];
  for (const line of low) {
    // KEYFORMAT and KEYFORMATVERSIONS need protocol version 5; AVPlayer refuses
    // the playlist as unparseable at the version ffmpeg writes.
    if (line.startsWith('#EXT-X-VERSION')) {
      out.push('#EXT-X-VERSION:5');
    } else if (line.startsWith('#EXT-X-TARGETDURATION')) {
      out.push(line, `#EXT-X-KEY:METHOD=SAMPLE-AES,URI="${FAIRPLAY_CONTENT_ID}",KEYFORMAT="com.apple.streamingkeydelivery",KEYFORMATVERSIONS="1"`);
    } else if (line.endsWith('.ts')) {
      out.push(`../vod/low/${line}`);
    } else {
      out.push(line);
    }
  }
  writeFileSync(join(dir, 'media.m3u8'), out.join('\n'));
  writeFileSync(join(dir, 'master.m3u8'),
    '#EXTM3U\n#EXT-X-VERSION:5\n#EXT-X-STREAM-INF:BANDWIDTH=450000,RESOLUTION=320x180,CODECS="avc1.4d4015,mp4a.40.2"\nmedia.m3u8\n');
  // Not an Apple-issued certificate: no key server will ever accept what is made
  // with it. The rows use it to see the certificate fetched and handed over.
  writeFileSync(join(dir, 'cert.der'), createHash('sha256').update('screenkit fairplay fixture').digest());
}

function makeCorrupt(root) {
  const dir = join(root, 'corrupt');
  mkdirSync(dir, { recursive: true });
  const ftyp = Buffer.from('000000186674797069736f6d0000020069736f6d69736f32', 'hex');
  // Deterministic garbage, so a failure reproduces.
  const garbage = Buffer.alloc(64 * 1024);
  let x = 0x12345678;
  for (let i = 0; i < garbage.length; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    garbage[i] = x >>> 24;
  }
  writeFileSync(join(dir, 'corrupt.mp4'), Buffer.concat([ftyp, garbage]));
}

/// Generate everything under <dir>/media. Returns a description the server
/// writes into servers.json.
export function prepareMedia(dir) {
  const root = join(dir, 'media');
  rmSync(root, { recursive: true, force: true });
  mkdirSync(root, { recursive: true });
  const started = Date.now();
  try {
    execFileSync(ffmpegBinary(), ['-hide_banner', '-version'], { stdio: 'ignore' });
  } catch {
    throw new Error(`media fixtures: no ffmpeg (${ffmpegBinary()}); install it or set FFMPEG=<path>`);
  }
  makeVod(root);
  makeLive(root);
  makeDash(root);
  makeMp4(root, 'mp4', '640x360', '800k', '3.0');
  makeMp4(root, 'big', '1280x720', '2000k', '3.1');
  makeHardware(root);
  makeCenc(root);
  makeFairPlay(root);
  makeCorrupt(root);
  return { root, seconds: (Date.now() - started) / 1000 };
}

// --- serving --------------------------------------------------------------------------

const TYPES = {
  '.m3u8': 'application/vnd.apple.mpegurl',
  '.mpd': 'application/dash+xml',
  '.mp4': 'video/mp4',
  '.m4s': 'video/iso.segment',
  '.ts': 'video/mp2t',
  '.vtt': 'text/vtt',
  '.der': 'application/octet-stream',
};

const liveStarted = Date.now();

// The live window: the newest LIVE_WINDOW segments at this moment. Segment k is
// file k mod LIVE_SEGMENTS, and a wrap is a discontinuity.
function livePlaylist() {
  const newest = LIVE_WINDOW + Math.floor((Date.now() - liveStarted) / (SEGMENT_SECONDS * 1000));
  const first = newest - LIVE_WINDOW;
  const lines = ['#EXTM3U', '#EXT-X-VERSION:3', `#EXT-X-TARGETDURATION:${SEGMENT_SECONDS}`,
                 `#EXT-X-MEDIA-SEQUENCE:${first}`, `#EXT-X-DISCONTINUITY-SEQUENCE:${Math.floor(first / LIVE_SEGMENTS)}`];
  for (let k = first; k < newest; k++) {
    if (k > first && k % LIVE_SEGMENTS === 0) lines.push('#EXT-X-DISCONTINUITY');
    lines.push(`#EXTINF:${SEGMENT_SECONDS.toFixed(3)},`, `seg/${k}.ts`);
  }
  return lines.join('\n') + '\n';
}

// What reached the fake licence server, per ?id=.
const licenceLog = new Map();

function sendFile(req, res, file) {
  const size = statSync(file).size;
  const type = TYPES[extname(file)] || 'application/octet-stream';
  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
  const headers = { 'content-type': type, 'accept-ranges': 'bytes', 'access-control-allow-origin': '*' };
  if (range && (range[1] !== '' || range[2] !== '')) {
    let start, end;
    if (range[1] === '') {
      start = Math.max(0, size - Number(range[2]));
      end = size - 1;
    } else {
      start = Number(range[1]);
      end = range[2] === '' ? size - 1 : Math.min(Number(range[2]), size - 1);
    }
    if (start >= size || start > end) {
      res.writeHead(416, { 'content-range': `bytes */${size}` });
      return res.end();
    }
    const body = readFileSync(file).subarray(start, end + 1);
    res.writeHead(206, { ...headers, 'content-range': `bytes ${start}-${end}/${size}`, 'content-length': body.length });
    return res.end(req.method === 'HEAD' ? undefined : body);
  }
  const body = readFileSync(file);
  res.writeHead(200, { ...headers, 'content-length': body.length });
  res.end(req.method === 'HEAD' ? undefined : body);
}

function readAll(req, done) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => done(Buffer.concat(chunks)));
}

/// Serve /media/... and /licence/...; false for any other path.
export function mediaRoute(req, res, url, root) {
  const path = url.pathname;
  const q = url.searchParams;

  // The fake licence server. `ok` answers 200 with fixed bytes, `fail` 500,
  // `slow` after a second; every request is recorded under ?id= for /licence/log.
  if (path.startsWith('/licence/')) {
    if (path === '/licence/log') {
      const body = Buffer.from(JSON.stringify(licenceLog.get(q.get('id') || '') || []));
      res.writeHead(200, { 'content-type': 'application/json', 'content-length': body.length });
      res.end(body);
      return true;
    }
    readAll(req, (body) => {
      const id = q.get('id') || '';
      if (!licenceLog.has(id)) licenceLog.set(id, []);
      licenceLog.get(id).push({ method: req.method, path: req.url, headers: req.headers,
                                length: body.length, body: body.toString('base64') });
      const answer = () => {
        if (path === '/licence/fail') {
          res.writeHead(500, { 'content-type': 'text/plain' });
          return res.end('licence refused');
        }
        const licence = Buffer.from('screenkit-fixture-licence');
        res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': licence.length });
        res.end(licence);
      };
      if (path === '/licence/slow') setTimeout(answer, 1000); else answer();
    });
    return true;
  }

  if (!path.startsWith('/media/')) return false;

  if (path === '/media/live/live.m3u8') {
    const body = Buffer.from(livePlaylist());
    res.writeHead(200, { 'content-type': TYPES['.m3u8'], 'content-length': body.length, 'cache-control': 'no-cache' });
    res.end(body);
    return true;
  }
  const seg = /^\/media\/live\/seg\/(\d+)\.ts$/.exec(path);
  if (seg) {
    const file = join(root, 'live', `seg${String(Number(seg[1]) % LIVE_SEGMENTS).padStart(3, '0')}.ts`);
    sendFile(req, res, file);
    return true;
  }

  // Everything else is a file under the media root, and never outside it.
  // A malformed escape (/media/%ZZ) is a 400, not a throw: nothing catches one
  // here, and the fixture server would take every later row down with it.
  let decoded;
  try {
    decoded = decodeURIComponent(path.slice('/media/'.length));
  } catch {
    res.writeHead(400, { 'content-type': 'text/plain' });
    res.end('bad media path');
    return true;
  }
  const relative = normalize(decoded);
  const file = join(root, relative);
  if (relative.startsWith('..') || !file.startsWith(root + sep) || !existsSync(file) || !statSync(file).isFile()) {
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('no such media');
    return true;
  }
  sendFile(req, res, file);
  return true;
}

// `node media.mjs <dir>` generates the set, for a look at it by hand.
if (import.meta.url === `file://${process.argv[1]}`) {
  const out = prepareMedia(process.argv[2] || '.');
  console.log(`media fixtures in ${out.root} (${out.seconds}s)`);
  for (const name of readdirSync(out.root)) console.log(`  ${name}/: ${readdirSync(join(out.root, name)).length} files`);
}

export { randomBytes };
