// Copyright (c) ScreenKit contributors. MIT.
//
// The seam. One `<video>` element's player, as an interface with one
// implementation per platform -- the platform's own player owns manifests,
// buffering, adaptive bitrate, decoding and presentation, which is what
// react-native-video does:
//
//   Apple    MediaPlayerApple.mm     AVPlayer / AVPlayerItem / AVPlayerLayer, FairPlay
//                                    through AVContentKeySession
//   Android  MediaPlayerAndroid.cpp  Media3 ExoPlayer over JNI
//            + dev/screenkit/media/VideoPlayer.java   (HLS, DASH, progressive; Widevine, ClearKey)
//   Linux    MediaPlayerLinux.cpp    libvlc (the image's VLC 3): demux, HLS/DASH, decode, A/V
//            + media/linux/          sync, ALSA audio; its pictures as dma-bufs on a Wayland
//                                    subsurface, decoded in hardware by the ScreenKit VLC plugin
//   other    MediaPlayerUnavailable.cpp   every load fails cleanly
//
// It is shaped like `net::NetService` on purpose: no JSI below it, events leave
// through a sink the binding implements by posting event-loop tasks (so they
// wait behind a paused runtime's freeze gate), and every entry point is safe
// from any thread -- an implementation posts to its own serial queue, or to the
// platform's main thread, and returns. None of them may block on the main
// thread (see `shutdown`), and none may call back into the sink synchronously
// from inside the call.
//
// `bindings/Media.cpp` is the only caller. Above it is `HTMLVideoElement` in
// the DOM shim, and above that `@screenkit/shaka`, a player whose API mirrors
// Shaka Player's (runtime/js/README.md, "Video").
//
// **Video never passes through the runtime's GL.** Each platform composites its
// own video plane *beneath* the app's drawable, at the element's CSS rect, and
// the app clears transparent where the video shows: an `AVPlayerLayer` beneath
// the metal view, a `SurfaceView` beneath SDL's, a Wayland subsurface beneath
// the window's surface.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;

namespace screenkit::media {

using Bytes = std::vector<std::uint8_t>;

/// A rectangle in the app's drawable pixels -- the space `gl.drawingBufferWidth`
/// and CSS pixels are measured in. `order` stacks two planes: a higher one sits
/// above a lower one, and every plane sits beneath the app's drawable.
struct PlaneRect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  int order = 0;
};

/// Adaptive bitrate, as Shaka's `abr` config holds it. Restrictions apply to
/// what ABR may choose; a variant picked with `selectVariant` is not restricted.
struct AbrConfig {
  bool enabled = true;
  double minBandwidth = 0;
  double maxBandwidth = std::numeric_limits<double>::infinity();
  int minWidth = 0;
  int maxWidth = std::numeric_limits<int>::max();
  int minHeight = 0;
  int maxHeight = std::numeric_limits<int>::max();
};

/// What JS configured for protected content (Shaka's `drm` config, reduced to
/// what native uses). The licence request itself is JS's: native raises
/// `onLicenceRequest` and waits for `provideLicence`.
struct DrmConfig {
  /// "com.widevine.alpha", "com.apple.fps", "org.w3.clearkey", or empty for none.
  std::string keySystem;
  /// The configured licence URL. Informational -- JS performs the request.
  std::string licenceServer;
  /// ClearKey keys, lower-case hex: 16-byte key id -> 16-byte key.
  std::vector<std::pair<std::string, std::string>> clearKeys;
  /// FairPlay's application certificate, or a Widevine service certificate.
  std::shared_ptr<const Bytes> serverCertificate;
  std::string videoRobustness;
  std::string audioRobustness;
};

struct LoadRequest {
  /// Stamped by the binding. Every event this load produces carries it back, so
  /// events from an earlier load that are still in flight are told apart.
  std::uint32_t serial = 0;
  std::string url;
  /// Seconds. NaN: the default -- the start for VOD, the live edge for live.
  double startTime = std::numeric_limits<double>::quiet_NaN();
  /// What JS was told or guessed: "application/x-mpegurl", "application/dash+xml",
  /// "video/mp4", ... or empty. A player uses it where its own sniffing cannot tell.
  std::string mimeType;
  DrmConfig drm;
  AbrConfig abr;
  /// Preferred languages (BCP 47), empty for the platform default.
  std::string audioLanguage;
  std::string textLanguage;
};

/// Where the load is, as HTMLMediaElement needs it. `Loading` until metadata and
/// the first frame are in; `Buffering` whenever playback could not advance for
/// lack of data (after a seek, a stall); `Ready` when it could; `Ended` at the
/// end of a VOD stream.
enum class PlaybackState { Loading = 0, Buffering = 1, Ready = 2, Ended = 3 };

struct MediaInfo {
  /// Seconds; +infinity for live.
  double duration = std::numeric_limits<double>::quiet_NaN();
  bool live = false;
  /// The seekable window, seconds on the media timeline.
  double seekStart = 0;
  double seekEnd = 0;
  int width = 0;
  int height = 0;
  bool hasVideo = false;
  bool hasAudio = false;
  /// "hls", "dash" or "progressive".
  std::string manifest;
};

struct TimeRange {
  double start = 0;
  double end = 0;
};

/// A playable combination, as Shaka lists variants. `id` is stable for a load.
struct VariantTrack {
  int id = 0;
  double bandwidth = 0;
  int width = 0;
  int height = 0;
  double frameRate = 0;
  std::string videoCodec;   // "avc1.64001f"
  std::string audioCodec;   // "mp4a.40.2"
  std::string language;     // of its audio
  std::string label;
  int audioId = -1;         // the AudioTrack it plays, -1 when none or unknown
  int channels = 0;
  bool active = false;
};

struct AudioTrack {
  int id = 0;
  std::string language;
  std::string label;
  std::string role;         // "main", "alternate", "commentary", ... or empty
  std::string codec;
  int channels = 0;
  bool active = false;
};

struct TextTrack {
  int id = 0;
  std::string language;
  std::string label;
  std::string kind;         // "subtitles" or "captions"
  std::string mimeType;     // "text/vtt", "application/ttml+xml", ...
  bool forced = false;
  bool active = false;
};

struct TrackList {
  std::vector<VariantTrack> variants;
  std::vector<AudioTrack> audio;
  std::vector<TextTrack> text;
};

/// A cue as the app draws it: plain text, with WebVTT's `\n` line breaks.
struct Cue {
  double start = 0;
  double end = 0;
  std::string text;
};

/// Counters for Shaka's `getStats()` and `getVideoPlaybackQuality()`. Frames
/// are since the load; bandwidths are bits per second, NaN when unknown.
struct MediaStats {
  int width = 0;
  int height = 0;
  double frameRate = 0;
  double streamBandwidth = std::numeric_limits<double>::quiet_NaN();
  double estimatedBandwidth = std::numeric_limits<double>::quiet_NaN();
  std::uint64_t decodedFrames = 0;
  std::uint64_t droppedFrames = 0;
  std::uint64_t corruptedFrames = 0;
};

/// Why a load or playback failed. The JS layers map these onto
/// `MediaError` codes and Shaka's `shaka.util.Error` (runtime/js/README.md).
/// **These numbers are a wire format** -- they cross JNI as plain ints
/// (VideoPlayer.java's ERR_*) -- so a new kind goes at the end.
enum class MediaErrorKind {
  /// The manifest or media could not be fetched. `httpStatus` is the response's
  /// status when there was one (Shaka 1001), 0 when there was none (1002).
  Network = 0,
  /// Not a manifest type this player plays -- DASH on Apple (Shaka 4000).
  Manifest = 1,
  /// Could not be demuxed or decoded: corrupt, or a codec this device lacks (3016).
  Media = 2,
  /// Nowhere to show video: no Wayland on Linux, a surface the platform refused (3016).
  VideoOutput = 3,
  /// The configured key system does not exist here (6001).
  KeySystem = 4,
  /// The key system refused the licence, or the licence exchange failed (6007).
  Licence = 5,
  /// This platform has no media player.
  Unavailable = 6,
};
inline constexpr int kMediaErrorKindCount = 7;

inline const char* mediaErrorKindName(MediaErrorKind kind) {
  switch (kind) {
    case MediaErrorKind::Network: return "network";
    case MediaErrorKind::Manifest: return "manifest";
    case MediaErrorKind::Media: return "media";
    case MediaErrorKind::VideoOutput: return "video-output";
    case MediaErrorKind::KeySystem: return "key-system";
    case MediaErrorKind::Licence: return "licence";
    case MediaErrorKind::Unavailable: return "unavailable";
  }
  return "media";
}

inline const char* playbackStateName(PlaybackState state) {
  switch (state) {
    case PlaybackState::Loading: return "loading";
    case PlaybackState::Buffering: return "buffering";
    case PlaybackState::Ready: return "ready";
    case PlaybackState::Ended: return "ended";
  }
  return "loading";
}

/// What a player emits, from any thread; the binding turns each call into an
/// event-loop task, delivered in order. `serial` is the `LoadRequest::serial` of
/// the load that produced the event -- captured when that load's item was made,
/// never read back from "whatever is current" -- so the binding can drop what an
/// earlier load still had in flight.
class MediaSink {
 public:
  virtual ~MediaSink() = default;

  /// Duration, size and the seekable window are known: HAVE_METADATA. Again
  /// whenever the duration or the live flag changes.
  virtual void onMetadata(std::uint32_t serial, MediaInfo info) = 0;
  virtual void onState(std::uint32_t serial, PlaybackState state) = 0;
  /// The playback position, at least every 250 ms while it advances, and after
  /// every state change. For live the seekable window moves with it.
  virtual void onTime(std::uint32_t serial, double position, double seekStart, double seekEnd) = 0;
  /// A `seek` finished; `position` is where playback now is.
  virtual void onSeeked(std::uint32_t serial, double position) = 0;
  virtual void onBuffered(std::uint32_t serial, std::vector<TimeRange> ranges) = 0;
  virtual void onTracks(std::uint32_t serial, TrackList tracks) = 0;
  /// The playing variant changed -- by `selectVariant`, or by ABR.
  virtual void onVariantChanged(std::uint32_t serial, int variantId) = 0;
  /// The decoded video size changed (videoWidth / videoHeight).
  virtual void onSize(std::uint32_t serial, int width, int height) = 0;
  /// The cues of the selected text track that are active now (possibly none).
  virtual void onCues(std::uint32_t serial, int textTrackId, std::vector<Cue> cues) = 0;
  /// The key system needs a licence: JS sends `challenge` to the licence server
  /// and answers with `provideLicence(requestId, ...)`. `contentId` is FairPlay's
  /// `skd://` identifier, empty elsewhere.
  virtual void onLicenceRequest(std::uint32_t serial, std::uint64_t requestId, std::string keySystem,
                                Bytes challenge, std::string contentId) = 0;
  virtual void onStats(std::uint32_t serial, MediaStats stats) = 0;
  /// Fatal for the load: playback stops. The player stays usable for another load.
  virtual void onError(std::uint32_t serial, MediaErrorKind kind, int httpStatus, std::string message) = 0;
};

struct MediaConfig {
  /// Log tag and thread names.
  std::string name = "screenkit";
  std::shared_ptr<MediaSink> sink;
};

class MediaPlayer {
 public:
  /// The platform's player, for one element. Never null: a platform with none
  /// returns the Unavailable player, whose every load fails `Unavailable`.
  static std::shared_ptr<MediaPlayer> create(MediaConfig config);

  virtual ~MediaPlayer() = default;

  MediaPlayer(const MediaPlayer&) = delete;
  MediaPlayer& operator=(const MediaPlayer&) = delete;

  /// Replace whatever was loaded. Paused until `play`.
  virtual void load(LoadRequest request) = 0;
  virtual void play() = 0;
  virtual void pause() = 0;
  /// Seconds on the media timeline; JS has already clamped to the seekable window.
  virtual void seek(double position) = 0;
  virtual void setRate(double rate) = 0;
  /// 0..1.
  virtual void setVolume(double volume) = 0;
  virtual void setMuted(bool muted) = 0;
  /// Where the plane goes, in drawable pixels; `visible` false hides it (display:
  /// none, removed from the document). Video is fitted inside the rect, aspect
  /// kept (`object-fit: contain`).
  virtual void setPlane(PlaneRect rect, bool visible) = 0;
  /// Play exactly this variant; ABR stays off until `setAbr` turns it back on.
  virtual void selectVariant(int variantId) = 0;
  virtual void setAbr(AbrConfig abr) = 0;
  virtual void selectAudioLanguage(std::string language, std::string role) = 0;
  /// -1 turns text off. A selected track's cues arrive through `onCues`.
  virtual void selectText(int textTrackId) = 0;
  /// The licence server's answer to `onLicenceRequest`; null (or empty) when the
  /// exchange failed, which fails the load with `Licence`.
  virtual void provideLicence(std::uint64_t requestId, std::shared_ptr<const Bytes> licence) = 0;
  /// Stop and release what was loaded; the player stays usable.
  virtual void unload() = 0;
  /// Release everything, the plane included. Synchronous and idempotent: nothing
  /// reaches the sink after it returns, and every other call is ignored.
  ///
  /// **It must not wait for the platform's main (UI) thread.** Runtime teardown
  /// calls it on the JS thread while the main thread is blocked joining that
  /// thread (`Runtime::shutdown`), so a synchronous hop to the main thread
  /// deadlocks. Drop the sink synchronously; release what only the main thread
  /// may touch (a layer, a view) with an asynchronous hop that owns what it needs.
  virtual void shutdown() = 0;

 protected:
  MediaPlayer() = default;
};

/// What this platform's player can do, for `canPlayType`, Shaka's
/// `isBrowserSupported`/`probeSupport`, and the early rejections the JS layer
/// makes itself (DASH on Apple is 4000 before anything is fetched).
struct MediaCapabilities {
  bool available = false;
  /// "apple", "android", "linux" or "none".
  std::string platform = "none";
  bool hls = false;
  bool dash = false;
  bool progressive = false;
  /// Key systems the player can use: "com.apple.fps", "com.widevine.alpha", "org.w3.clearkey".
  std::vector<std::string> keySystems;
  /// Container MIME types it can play directly: "video/mp4", "video/webm", ...
  std::vector<std::string> containers;
  /// RFC 6381 codec prefixes it can decode: "avc1", "hvc1", "mp4a", "vp09", ...
  std::vector<std::string> codecs;
  /// False when there is nowhere to show video (Linux without Wayland); every
  /// load then fails `VideoOutput` with `videoOutputProblem` as its message.
  bool videoOutput = false;
  std::string videoOutputProblem;
};

MediaCapabilities mediaCapabilities();

/// False on a platform with no player, where every load fails `Unavailable`.
bool mediaAvailable();

// ---- where video goes --------------------------------------------------------

/// The app's window, as the platform shell made it. The host sets it once the
/// window and its drawable exist and clears it before they go (main thread); a
/// player reads it whenever it places a plane. With no host -- the headless
/// test rows -- a player still plays, and its plane goes nowhere.
struct VideoHost {
  SDL_Window* window = nullptr;
  /// Apple: the `SDL_MetalView` (an NSView or UIView) the app's CAMetalLayer
  /// belongs to -- video goes beneath it. Unused elsewhere.
  void* view = nullptr;
  /// The drawable a plane is measured in, when it is a fixed size scaled into
  /// the window (Linux, Android: GlSurface's `fixedWidth`). 0: the window's own
  /// pixel size.
  int drawableWidth = 0;
  int drawableHeight = 0;
  /// The display's own mode, read where SDL asks it to be read -- on the main
  /// thread, when this is set. A player picking a ceiling (MediaPlayerLinux)
  /// runs on its own thread and reads it from here instead. 0: unknown.
  int displayWidth = 0;
  int displayHeight = 0;
};

void setVideoHost(const VideoHost& host);
void clearVideoHost();
VideoHost videoHost();

/// `plane`, in drawable pixels, in the window's pixels: the identity for a
/// drawable that is the window, else the scale-and-centre GlSurfaceSdl presents
/// a fixed-size frame with, so a plane lands exactly under its CSS rect.
PlaneRect planeInWindow(const PlaneRect& plane, int windowWidth, int windowHeight, int drawableWidth,
                        int drawableHeight);

/// The rect `object-fit: contain` puts a `videoWidth` x `videoHeight` picture in
/// inside `box` -- for a platform whose surface has no aspect-fit of its own.
PlaneRect fitContain(const PlaneRect& box, int videoWidth, int videoHeight);

}  // namespace screenkit::media
