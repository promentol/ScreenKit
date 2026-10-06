// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

class EventLoop;
class MediaBinding;
class MediaRegistry;

/// The live players of one runtime, reachable from any thread -- which is what
/// the lifecycle needs: a runtime that pauses pauses its players from whatever
/// thread paused it, without waiting for a JS thread that is frozen. Made with
/// the host, before its JS thread exists.
std::shared_ptr<MediaRegistry> makeMediaRegistry();

/// `Architecture.md` 5.1: a `Paused` runtime's video is stopped. Pauses every
/// player the app has playing, and on resume plays them again, so the page sees
/// no change of its own `paused` state. Any thread; idempotent.
void setMediaRuntimePaused(MediaRegistry& registry, bool paused);

/// Install `__screenkit.media`: the narrow handle API `HTMLVideoElement` is
/// built on (runtime/js/README.md, "Video"), over `media::MediaPlayer`.
///
///   capabilities() -> {available, platform, hls, dash, progressive, keySystems,
///                      containers, codecs, videoOutput, videoOutputProblem}
///   onevent = function (target, type, payload) {...}      set by the shim
///   create(target) -> id          one player; events go to `target`
///   load(id, {url, asset?, startTime?, mimeType?, drm?, abr?, audioLanguage?,
///             textLanguage?}) -> serial
///   play(id) · pause(id) · seek(id, t) · setRate(id, r) · setVolume(id, v)
///   setMuted(id, b) · setPlane(id, x, y, width, height, visible, order)
///   selectVariant(id, variant) · setAbr(id, {...}) · selectAudioLanguage(id, lang, role)
///   selectText(id, track) · provideLicence(id, requestId, bytes|null)
///   unload(id) -> serial · destroy(id)
///
///   events, each payload carrying the `serial` of the load it belongs to:
///     metadata {duration, live, seekStart, seekEnd, width, height, hasVideo, hasAudio, manifest}
///     state {state: 'loading'|'buffering'|'ready'|'ended'}
///     time {position, seekStart, seekEnd} · seeked {position}
///     buffered {ranges: [[start, end]...]} · size {width, height}
///     tracks {variants, audio, text} · variant {id}
///     cues {track, cues: [{start, end, text}]}
///     licence {requestId, keySystem, challenge: ArrayBuffer, contentId}
///     stats {width, height, frameRate, streamBandwidth, estimatedBandwidth,
///            decodedFrames, droppedFrames, corruptedFrames}
///     error {kind, httpStatus, message}
///
/// `url` is http: or https:. A package asset is named by `asset` -- the path
/// inside the package, which the shim splits out of a `screenkit:` URL -- and
/// reaches the player as the confined file it resolves to, exactly as `readFile`
/// confines it; `url` then only says what the page asked for. A `screenkit:` URL
/// passed here with no `asset` is refused like any other unsupported scheme.
///
/// Events arrive as event-loop tasks through `executor`, in order per player,
/// behind the freeze gate. Only the latest load's (or unload's) serial is
/// delivered: whatever an earlier load still had in flight is dropped here. The
/// player is released by `destroy`, when `target` is collected, or at shutdown;
/// until then the runtime's loop is held non-idle.
///
/// `__screenkit` must already exist (installHostIO). JS thread only.
std::shared_ptr<MediaBinding> installMedia(facebook::jsi::Runtime& runtime, std::shared_ptr<JsExecutor> executor,
                                           std::shared_ptr<EventLoop> loop,
                                           std::shared_ptr<MediaRegistry> registry,
                                           const RuntimeConfig& config);

/// Shut every player down -- synchronously, so nothing is delivered afterwards --
/// and drop every JS reference, while the runtime still exists. JS thread only;
/// idempotent.
void shutdownMedia(MediaBinding& binding);

}  // namespace screenkit
