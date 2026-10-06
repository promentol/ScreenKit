// Copyright (c) ScreenKit contributors. MIT.
//
// The Apple player (macOS, tvOS): AVPlayer owns the manifest, buffering, ABR,
// decoding and presentation, as it does in react-native-video. What is here is
// translation -- AVFoundation's state into MediaSink events, and the seam's
// calls into AVPlayer's -- plus the plane: an AVPlayerLayer in a view of its
// own, inserted *beneath* the SDL metal view the app draws in, so video never
// passes through the runtime's GL.
//
// Threads. Every AVPlayer call and every piece of player state lives on one
// serial dispatch queue per player; a seam call posts there and returns. Views
// are the main thread's, reached only with dispatch_async: the main thread may
// be blocked joining the JS thread while the runtime shuts down (MediaPlayer.h,
// `shutdown`), so nothing here ever waits for it. The state machine is a poll of
// the item on that queue, not KVO: KVO on AVPlayerItem arrives on whatever
// thread AVFoundation chooses, and the headless test rows have no main run loop
// to deliver anything that goes through the main queue.
//
// Frames. AVPlayer has no public decoded-frame counter (AVVideoPerformanceMetrics'
// counters are SPI), so an AVPlayerItemVideoOutput rides along with every item
// and the poll counts the new pixel buffers it vends while playing -- the frames
// that were decoded and reached presentation time. Dropped frames are the access
// log's `c-frames-dropped`. The output also makes AVPlayer decode when no layer
// is attached, which is what the headless rows measure.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <screenkit/Log.h>

#include "MediaPlayer.h"

using screenkit::LogLevel;
using namespace screenkit::media;

namespace {

constexpr const char* kTag = "screenkit.media";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
/// How long a FairPlay key request waits for JS to answer it, the same wait
/// Android gives one (VideoPlayer.java, LICENCE_TIMEOUT_MS).
constexpr double kLicenceTimeoutSeconds = 30.0;

double secondsOf(CMTime time) {
  if (CMTIME_IS_INDEFINITE(time)) return kInf;
  if (!CMTIME_IS_NUMERIC(time)) return kNaN;
  return CMTimeGetSeconds(time);
}

std::string utf8(NSString* text) { return text == nil ? std::string() : std::string(text.UTF8String ?: ""); }

NSString* nsString(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()] ?: @""; }

std::string fourCC(FourCharCode code) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = static_cast<char>((code >> shift) & 0xff);
    if (c != ' ' && c != 0) out += c;
  }
  return out;
}

/// An AudioFormatID as the RFC 6381 prefix a codecs string would use.
std::string audioCodecName(FourCharCode format) {
  switch (format) {
    case kAudioFormatMPEG4AAC:
    case kAudioFormatMPEG4AAC_HE:
    case kAudioFormatMPEG4AAC_HE_V2:
    case kAudioFormatMPEG4AAC_LD:
    case kAudioFormatMPEG4AAC_ELD:
      return "mp4a";
    case kAudioFormatAC3: return "ac-3";
    case kAudioFormatEnhancedAC3: return "ec-3";
    case kAudioFormatOpus: return "opus";
    case kAudioFormatFLAC: return "flac";
    case kAudioFormatAppleLossless: return "alac";
    case kAudioFormatMPEGLayer3: return "mp3";
    default: return fourCC(format);
  }
}

std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool isFairPlay(const std::string& keySystem) { return keySystem.rfind("com.apple.fps", 0) == 0; }

/// "HTTP 404" anywhere in an error, its failure reason, its underlying errors
/// or the error log's comment: CoreMedia reports an HTTP failure as a comment
/// ("HTTP 404: File Not Found") on a generic code, never as a status field.
int httpStatusIn(NSString* text) {
  if (text.length == 0) return 0;
  NSRegularExpression* pattern = [NSRegularExpression regularExpressionWithPattern:@"HTTP[ /:]*([1-5][0-9][0-9])"
                                                                           options:0
                                                                             error:nil];
  NSTextCheckingResult* match = [pattern firstMatchInString:text options:0 range:NSMakeRange(0, text.length)];
  if (match == nil) return 0;
  return [[text substringWithRange:[match rangeAtIndex:1]] intValue];
}

/// CoreMedia's own codes for the HTTP failures it names -- what a progressive
/// download reports, with no error-log comment to read the status from.
int httpStatusForCode(NSInteger code) {
  switch (code) {
    case -12938: return 404;  // "HTTP 404: File Not Found"
    case -12660: return 403;  // "HTTP 403: Forbidden"
    default: return 0;
  }
}

int httpStatusInError(NSError* error, int depth = 0) {
  if (error == nil || depth > 6) return 0;
  if ([error.domain isEqualToString:NSOSStatusErrorDomain] || [error.domain isEqualToString:@"CoreMediaErrorDomain"]) {
    const int status = httpStatusForCode(error.code);
    if (status != 0) return status;
  }
  for (NSString* text in @[ error.localizedDescription ?: @"", error.localizedFailureReason ?: @"",
                            error.userInfo[NSDebugDescriptionErrorKey] ?: @"" ]) {
    if (![text isKindOfClass:[NSString class]]) continue;
    const int status = httpStatusIn(text);
    if (status != 0) return status;
  }
  return httpStatusInError(error.userInfo[NSUnderlyingErrorKey], depth + 1);
}

std::string describe(NSError* error) {
  if (error == nil) return "unknown error";
  std::string out = utf8(error.localizedDescription);
  NSError* underlying = error.userInfo[NSUnderlyingErrorKey];
  if (underlying != nil) out += " (" + utf8(underlying.domain) + " " + std::to_string(underlying.code) + ")";
  if (error.localizedFailureReason.length > 0) out += ": " + utf8(error.localizedFailureReason);
  return out;
}

bool urlErrorIn(NSError* error, int depth = 0) {
  if (error == nil || depth > 6) return false;
  if ([error.domain isEqualToString:NSURLErrorDomain]) return true;
  return urlErrorIn(error.userInfo[NSUnderlyingErrorKey], depth + 1);
}

// ---- WebVTT samples ----------------------------------------------------------------
// AVFoundation hands WebVTT cues over as ISO/IEC 14496-30 samples when asked for
// the native representation: a sample is a run of boxes, a `vttc` per active
// cue carrying its text in `payl`, or a `vtte` when nothing is active. Reading
// them, rather than the styled NSAttributedStrings, is what gives a cue its end
// time: the sample's own duration.

uint32_t readBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void vttPayloads(const uint8_t* data, size_t size, std::vector<std::string>& out) {
  size_t at = 0;
  while (at + 8 <= size) {
    const uint32_t boxSize = readBE32(data + at);
    if (boxSize < 8 || at + boxSize > size) return;
    const std::string type(reinterpret_cast<const char*>(data + at + 4), 4);
    if (type == "vttc") {
      // Its children: iden, sttg, payl.
      size_t inner = at + 8;
      while (inner + 8 <= at + boxSize) {
        const uint32_t childSize = readBE32(data + inner);
        if (childSize < 8 || inner + childSize > at + boxSize) break;
        if (std::string(reinterpret_cast<const char*>(data + inner + 4), 4) == "payl") {
          out.emplace_back(reinterpret_cast<const char*>(data + inner + 8), childSize - 8);
        }
        inner += childSize;
      }
    }
    at += boxSize;
  }
}

}  // namespace

// ---- the plane: main thread only -----------------------------------------------------

#if TARGET_OS_OSX
@interface SKVideoContainer : NSView
@property(nonatomic) int order;
@property(nonatomic) NSUInteger ordinal;
@end
@implementation SKVideoContainer
// Input is the app's, not the video's: the metal view above already refuses it.
- (NSView*)hitTest:(NSPoint)point {
  (void)point;
  return nil;
}
@end
#else
@interface SKVideoContainer : UIView
@property(nonatomic) int order;
@property(nonatomic) NSUInteger ordinal;
@end
@implementation SKVideoContainer
+ (Class)layerClass {
  return [AVPlayerLayer class];
}
@end
#endif

namespace {

/// How many visible planes sit beneath each metal layer, and what the layer and
/// its window looked like before the first. The layer is non-opaque only while
/// at least one plane is visible: an opaque layer lets the compositor skip
/// whatever is beneath it, so the canvas-only path costs exactly what it did
/// before video. While it is not, the window behind is black -- what a page's
/// transparent pixels away from the video show, rather than the desktop or the
/// tvOS home screen.
struct LayerOpacity {
  int visible = 0;
  bool originalOpaque = true;
#if TARGET_OS_OSX
  NSColor* originalBackground = nil;
  __weak NSWindow* window = nil;
#else
  UIColor* originalBackground = nil;
  __weak UIWindow* window = nil;
#endif
};
std::map<void*, LayerOpacity>& opacityTable() {
  static std::map<void*, LayerOpacity> table;  // main thread only
  return table;
}

#if TARGET_OS_OSX
void planeShown(CALayer* metal, NSView* view) {
#else
void planeShown(CALayer* metal, UIView* view) {
#endif
  if (metal == nil) return;
  LayerOpacity& entry = opacityTable()[(__bridge void*)metal];
  if (entry.visible++ == 0) {
    entry.originalOpaque = metal.opaque;
    metal.opaque = NO;
    entry.window = view.window;
    entry.originalBackground = view.window.backgroundColor;
#if TARGET_OS_OSX
    view.window.backgroundColor = NSColor.blackColor;
#else
    view.window.backgroundColor = UIColor.blackColor;
#endif
  }
}

void planeHidden(CALayer* metal) {
  if (metal == nil) return;
  auto it = opacityTable().find((__bridge void*)metal);
  if (it == opacityTable().end()) return;
  if (--it->second.visible <= 0) {
    metal.opaque = it->second.originalOpaque;
    it->second.window.backgroundColor = it->second.originalBackground;
    opacityTable().erase(it);
  }
}

NSUInteger nextOrdinal() {
  static NSUInteger ordinal = 0;  // main thread only
  return ++ordinal;
}

}  // namespace

/// One player's plane. Every method runs on the main thread.
@interface SKPlaneHost : NSObject
- (instancetype)initWithPlayer:(AVPlayer*)player;
- (void)applyRect:(PlaneRect)rect visible:(BOOL)visible view:(void*)view;
- (void)detach;
@end

@implementation SKPlaneHost {
  AVPlayer* _player;
  AVPlayerLayer* _layer;
  SKVideoContainer* _container;
#if TARGET_OS_OSX
  __weak NSView* _attached;
#else
  __weak UIView* _attached;
#endif
  __weak CALayer* _metal;
  BOOL _shown;
}

- (instancetype)initWithPlayer:(AVPlayer*)player {
  self = [super init];
  if (self != nil) _player = player;
  return self;
}

- (void)makeContainer {
  if (_container != nil) return;
#if TARGET_OS_OSX
  _layer = [AVPlayerLayer playerLayerWithPlayer:_player];
  _layer.videoGravity = AVLayerVideoGravityResizeAspect;
  _container = [[SKVideoContainer alloc] initWithFrame:NSZeroRect];
  // Layer-hosting: the AVPlayerLayer is the view's layer, so it always fills it.
  _container.layer = _layer;
  _container.wantsLayer = YES;
#else
  _container = [[SKVideoContainer alloc] initWithFrame:CGRectZero];
  _container.userInteractionEnabled = NO;
  _container.backgroundColor = nil;
  _layer = (AVPlayerLayer*)_container.layer;
  _layer.player = _player;
  _layer.videoGravity = AVLayerVideoGravityResizeAspect;
#endif
  _container.ordinal = nextOrdinal();
}

/// Every video container beneath the metal view, lowest `order` first, each put
/// directly under the metal view in turn -- so the last is the highest.
- (void)restack {
#if TARGET_OS_OSX
  NSView* metal = _attached;
  NSView* parent = metal.superview;
#else
  UIView* metal = _attached;
  UIView* parent = metal.superview;
#endif
  if (metal == nil || parent == nil) return;
  NSMutableArray<SKVideoContainer*>* planes = [NSMutableArray array];
  for (id view in parent.subviews) {
    if ([view isKindOfClass:[SKVideoContainer class]]) [planes addObject:view];
  }
  if (_container != nil && ![planes containsObject:_container]) [planes addObject:_container];
  [planes sortUsingComparator:^NSComparisonResult(SKVideoContainer* a, SKVideoContainer* b) {
    if (a.order != b.order) return a.order < b.order ? NSOrderedAscending : NSOrderedDescending;
    return a.ordinal < b.ordinal ? NSOrderedAscending : NSOrderedDescending;
  }];
  for (SKVideoContainer* plane in planes) {
#if TARGET_OS_OSX
    [parent addSubview:plane positioned:NSWindowBelow relativeTo:metal];
#else
    [parent insertSubview:plane belowSubview:metal];
#endif
  }
}

- (void)applyRect:(PlaneRect)rect visible:(BOOL)visible view:(void*)view {
  // The host may have gone between the post and now: the window is closing.
  if (view == nullptr || videoHost().view != view) {
    [self detach];
    return;
  }
#if TARGET_OS_OSX
  NSView* metal = (__bridge NSView*)view;
#else
  UIView* metal = (__bridge UIView*)view;
#endif
  if (metal.superview == nil) {
    [self detach];
    return;
  }
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  BOOL restack = NO;
  if (_attached != metal) {
    [self detach];
    [self makeContainer];
    _attached = metal;
    _metal = metal.layer;
    restack = YES;
  }
  if (_container.order != rect.order) {
    _container.order = rect.order;
    restack = YES;
  }
  if (restack) [self restack];

  // Drawable pixels to the metal view's points, then to its superview's.
  const CGFloat scale = metal.layer.contentsScale > 0 ? metal.layer.contentsScale : 1;
  const CGRect inMetal = CGRectMake(rect.x / scale, rect.y / scale, rect.width / scale, rect.height / scale);
#if TARGET_OS_OSX
  // NSView is bottom-left unless flipped; a plane is top-left, as CSS is.
  const CGRect local = metal.isFlipped
                           ? inMetal
                           : CGRectMake(inMetal.origin.x, metal.bounds.size.height - inMetal.origin.y - inMetal.size.height,
                                        inMetal.size.width, inMetal.size.height);
  _container.frame = [metal convertRect:local toView:metal.superview];
#else
  _container.frame = [metal convertRect:inMetal toView:metal.superview];
#endif
  _container.hidden = !visible;
  if (visible && !_shown) {
    planeShown(_metal, metal);
    _shown = YES;
  } else if (!visible && _shown) {
    planeHidden(_metal);
    _shown = NO;
  }
  [CATransaction commit];
}

- (void)detach {
  if (_shown) {
    planeHidden(_metal);
    _shown = NO;
  }
  [_container removeFromSuperview];
  _attached = nil;
  _metal = nil;
}

@end

// ---- the player's core: its serial queue --------------------------------------------

@interface SKMediaCore : NSObject <AVPlayerItemLegibleOutputPushDelegate, AVContentKeySessionDelegate>
- (instancetype)initWithName:(const std::string&)name sink:(std::shared_ptr<MediaSink>)sink;
- (void)load:(std::shared_ptr<LoadRequest>)request;
- (void)play;
- (void)pause;
- (void)seek:(double)position;
- (void)setRate:(double)rate;
- (void)setVolume:(double)volume;
- (void)setMuted:(BOOL)muted;
- (void)setPlane:(PlaneRect)rect visible:(BOOL)visible;
- (void)selectVariant:(int)variantId;
- (void)setAbr:(AbrConfig)abr;
- (void)selectAudioLanguage:(std::string)language role:(std::string)role;
- (void)selectText:(int)textId;
- (void)provideLicence:(uint64_t)requestId licence:(std::shared_ptr<const Bytes>)licence;
- (void)unload;
- (void)shutdown;
@end

@implementation SKMediaCore {
  std::string _name;
  dispatch_queue_t _queue;
  NSOperationQueue* _notifications;

  // Guarded by _sinkMutex: an event is posted under it, so once shutdown has
  // swapped the sink out, nothing reaches it -- not even an event in flight.
  std::mutex _sinkMutex;
  std::shared_ptr<MediaSink> _sink;
  std::atomic<bool> _stopped;

  // Everything below is the queue's.
  AVPlayer* _player;
  AVURLAsset* _asset;
  AVPlayerItem* _item;
  AVPlayerItemVideoOutput* _videoOutput;
  AVPlayerItemLegibleOutput* _legibleOutput;
  AVContentKeySession* _keySession;
  NSMutableArray* _observers;
  dispatch_source_t _timer;
  double _timerInterval;

  std::uint32_t _serial;
  LoadRequest _request;
  bool _loaded;          // an item is loaded and has not failed
  bool _failed;
  bool _metadataSent;
  bool _assetKeysLoaded;
  bool _tracksSent;
  bool _ended;
  bool _wantsPlay;
  bool _startPending;    // a start position is being sought before playing
  double _lastWindowStart;  // the live window at the last time report
  double _lastWindowEnd;
  double _rate;
  double _readyAtNs;
  PlaybackState _state;
  bool _stateSent;
  double _lastTimeSent;
  double _lastTimeNs;
  double _lastSlowNs;
  double _lastStatsNs;
  double _lastBufferedNs;
  std::vector<TimeRange> _buffered;
  MediaInfo _info;
  int _width;
  int _height;

  // Seeks: only the latest one's completion is reported.
  std::uint64_t _seekGeneration;
  bool _seeking;
  double _pendingSeek;   // a seek asked for before metadata

  // Tracks.
  NSArray* _variants;    // AVAssetVariant, in the multivariant playlist's order
  AVMediaSelectionGroup* _legibleGroup;
  AVMediaSelectionGroup* _audibleGroup;
  int _activeVariant;
  int _pinnedVariant;
  AbrConfig _abr;
  int _selectedText;
  std::vector<Cue> _cues;
  double _cueOffset;     // stream clock minus item clock, for WebVTT samples

  // Stats.
  std::uint64_t _decodedFrames;

  // FairPlay.
  std::map<std::uint64_t, AVContentKeyRequest*> _keyRequests;
  std::uint64_t _nextRequestId;
  bool _keyRequested;

  // The plane.
  SKPlaneHost* _plane;
  PlaneRect _planeRect;
  bool _planeVisible;
  bool _planeSet;
}

- (instancetype)initWithName:(const std::string&)name sink:(std::shared_ptr<MediaSink>)sink {
  self = [super init];
  if (self == nil) return nil;
  _name = name;
  _sink = std::move(sink);
  _stopped = false;
  _queue = dispatch_queue_create((name + ".media").c_str(), DISPATCH_QUEUE_SERIAL);
  _notifications = [[NSOperationQueue alloc] init];
  _notifications.maxConcurrentOperationCount = 1;
  _notifications.underlyingQueue = _queue;
  _observers = [NSMutableArray array];
  _player = [[AVPlayer alloc] init];
  // Selection is the page's: nothing is picked from the user's accessibility
  // preferences behind its back, and a text track stays off until selected.
  _player.appliesMediaSelectionCriteriaAutomatically = NO;
  _player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
#if TARGET_OS_OSX
  _player.preventsDisplaySleepDuringVideoPlayback = YES;
#endif
  _rate = 1.0;
  _pendingSeek = kNaN;
  _pinnedVariant = -1;
  _selectedText = -1;
  _activeVariant = -1;
  _nextRequestId = 1;
  return self;
}

// ---- events ------------------------------------------------------------------------

- (void)emit:(void (^)(MediaSink& sink))event {
  std::lock_guard<std::mutex> lock(_sinkMutex);
  if (_sink) event(*_sink);
}

- (void)fail:(MediaErrorKind)kind status:(int)status message:(const std::string&)message {
  if (_failed) return;
  _failed = true;
  _loaded = false;
  [self stopTimer];
  if (_player.rate != 0) [_player pause];
  const std::uint32_t serial = _serial;
  screenkit::log(LogLevel::Warn, kTag,
                 std::string("load failed (") + mediaErrorKindName(kind) + (status ? " " + std::to_string(status) : "") +
                     "): " + message);
  [self emit:^(MediaSink& sink) { sink.onError(serial, kind, status, message); }];
}

- (void)failWithError:(NSError*)error {
  NSString* comment = nil;
  AVPlayerItemErrorLogEvent* last = _item.errorLog.events.lastObject;
  if (last != nil) comment = last.errorComment;
  int status = httpStatusIn(comment);
  if (status == 0) status = httpStatusInError(error);
  std::string message = describe(error);
  if (comment.length > 0) message += " [" + utf8(comment) + "]";
  if (status != 0) {
    [self fail:MediaErrorKind::Network status:status message:message];
  } else if (_keyRequested) {
    // Whatever failed after FairPlay was asked for a key is the key's failure:
    // AVFoundation reports a refused CKC on the item, not on the key request.
    [self fail:MediaErrorKind::Licence status:0 message:message];
  } else if (urlErrorIn(error)) {
    [self fail:MediaErrorKind::Network status:0 message:message];
  } else {
    [self fail:MediaErrorKind::Media status:0 message:message];
  }
}

// ---- the seam's calls, each posted to the queue ------------------------------------------

- (void)onQueue:(dispatch_block_t)work {
  if (_stopped.load()) return;
  __weak SKMediaCore* weakSelf = self;
  dispatch_async(_queue, ^{
    SKMediaCore* strong = weakSelf;
    if (strong == nil || strong->_stopped.load()) return;
    work();
  });
}

- (void)load:(std::shared_ptr<LoadRequest>)request {
  [self onQueue:^{ [self startLoad:*request]; }];
}

- (void)play {
  [self onQueue:^{
    self->_wantsPlay = true;
    if (self->_ended && self->_loaded) {
      // Played again at the end: from the start, as a browser does.
      [self seekInternal:0 report:false];
    }
    [self applyRate];
  }];
}

- (void)pause {
  [self onQueue:^{
    self->_wantsPlay = false;
    [self applyRate];
    [self sendTime:true];
  }];
}

- (void)seek:(double)position {
  [self onQueue:^{ [self seekInternal:position report:true]; }];
}

- (void)setRate:(double)rate {
  [self onQueue:^{
    self->_rate = rate;
    [self applyRate];
  }];
}

- (void)setVolume:(double)volume {
  [self onQueue:^{ self->_player.volume = static_cast<float>(volume); }];
}

- (void)setMuted:(BOOL)muted {
  [self onQueue:^{ self->_player.muted = muted; }];
}

- (void)setPlane:(PlaneRect)rect visible:(BOOL)visible {
  [self onQueue:^{
    self->_planeRect = rect;
    self->_planeVisible = visible;
    self->_planeSet = true;
    [self placePlane];
  }];
}

- (void)selectVariant:(int)variantId {
  [self onQueue:^{
    self->_pinnedVariant = variantId;
    self->_abr.enabled = false;
    [self applyAbr];
  }];
}

- (void)setAbr:(AbrConfig)abr {
  [self onQueue:^{
    self->_abr = abr;
    if (abr.enabled) self->_pinnedVariant = -1;
    [self applyAbr];
  }];
}

- (void)selectAudioLanguage:(std::string)language role:(std::string)role {
  auto shared = std::make_shared<std::pair<std::string, std::string>>(std::move(language), std::move(role));
  [self onQueue:^{ [self applyAudioLanguage:shared->first role:shared->second]; }];
}

- (void)selectText:(int)textId {
  [self onQueue:^{
    self->_selectedText = textId;
    [self applyText];
  }];
}

- (void)provideLicence:(uint64_t)requestId licence:(std::shared_ptr<const Bytes>)licence {
  [self onQueue:^{ [self answerKeyRequest:requestId licence:licence]; }];
}

- (void)unload {
  [self onQueue:^{ [self teardownItem]; }];
}

- (void)shutdown {
  if (_stopped.exchange(true)) return;
  {
    // After this, nothing reaches the sink: every event is posted under the lock.
    std::lock_guard<std::mutex> lock(_sinkMutex);
    _sink.reset();
  }
  // The rest is asynchronous and owns what it needs: the queue keeps `self`
  // alive until the item is gone, and the plane goes on the main thread when it
  // next runs -- never waited for, since it may be blocked joining the JS thread.
  SKMediaCore* strong = self;
  dispatch_async(_queue, ^{
    [strong teardownItem];
    [strong->_player replaceCurrentItemWithPlayerItem:nil];
    SKPlaneHost* plane = strong->_plane;
    strong->_plane = nil;
    if (plane != nil) {
      dispatch_async(dispatch_get_main_queue(), ^{ [plane detach]; });
    }
  });
}

// ---- loading -------------------------------------------------------------------------

- (void)startLoad:(const LoadRequest&)request {
  [self teardownItem];
  _request = request;
  _serial = request.serial;
  _failed = false;
  _metadataSent = false;
  _assetKeysLoaded = false;
  _tracksSent = false;
  _ended = false;
  _stateSent = false;
  _startPending = false;
  _seeking = false;
  _pendingSeek = kNaN;
  _state = PlaybackState::Loading;
  _lastTimeSent = kNaN;
  _lastWindowStart = kNaN;
  _lastWindowEnd = kNaN;
  _lastTimeNs = 0;
  _lastSlowNs = 0;
  _lastStatsNs = 0;
  _lastBufferedNs = 0;
  _buffered.clear();
  _info = MediaInfo();
  _width = 0;
  _height = 0;
  _variants = nil;
  _legibleGroup = nil;
  _audibleGroup = nil;
  _activeVariant = -1;
  _pinnedVariant = -1;
  _abr = request.abr;
  _selectedText = -1;
  _cues.clear();
  _cueOffset = kNaN;
  _decodedFrames = 0;
  _keyRequested = false;
  _wantsPlay = false;
  _readyAtNs = 0;

  const std::uint32_t serial = _serial;
  NSURL* url = [NSURL URLWithString:nsString(request.url)];
  if (url == nil) {
    [self fail:MediaErrorKind::Media status:0 message:"not a URL AVPlayer can open: " + request.url];
    return;
  }
  // AVPlayer has no DASH (spec-video-player.md: a recorded divergence).
  const std::string path = lower(utf8(url.path));
  const std::string mime = lower(request.mimeType);
  if ((path.size() > 4 && path.compare(path.size() - 4, 4, ".mpd") == 0) || mime.find("dash") != std::string::npos) {
    [self fail:MediaErrorKind::Manifest
          status:0
         message:"AVPlayer does not play DASH; HLS and MP4 play on Apple (" + request.url + ")"];
    return;
  }
  // FairPlay is the one key system here.
  if ((!request.drm.keySystem.empty() && !isFairPlay(request.drm.keySystem)) || !request.drm.clearKeys.empty()) {
    const std::string system = request.drm.keySystem.empty() ? "org.w3.clearkey" : request.drm.keySystem;
    [self fail:MediaErrorKind::KeySystem
          status:0
         message:system + " is not available on Apple platforms: FairPlay (com.apple.fps) is the key system here"];
    return;
  }

  [self emit:^(MediaSink& sink) { sink.onState(serial, PlaybackState::Loading); }];
  _stateSent = true;

  _asset = [AVURLAsset URLAssetWithURL:url options:nil];
  if (isFairPlay(request.drm.keySystem)) {
    // A simulator has no FairPlay: AVContentKeySession throws rather than fails.
    NSString* why = nil;
#if TARGET_OS_SIMULATOR
    why = @"FairPlay Streaming is not supported on simulators";
#else
    @try {
      _keySession = [AVContentKeySession contentKeySessionWithKeySystem:AVContentKeySystemFairPlayStreaming];
    } @catch (NSException* exception) {
      why = exception.reason ?: exception.name;
    }
#endif
    if (_keySession == nil) {
      _asset = nil;
      [self fail:MediaErrorKind::KeySystem status:0 message:"com.apple.fps is not available here: " + utf8(why)];
      return;
    }
    [_keySession setDelegate:self queue:_queue];
    [_keySession addContentKeyRecipient:_asset];
  }

  _item = [AVPlayerItem playerItemWithAsset:_asset];
  [self applyAbr];
  // The decoder's own formats, so vending a frame to the counter is a retain
  // and never a conversion.
  _videoOutput = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:@{
    (NSString*)kCVPixelBufferPixelFormatTypeKey : @[
      @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
      @(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange), @(kCVPixelFormatType_420YpCbCr10BiPlanarFullRange)
    ]
  }];
  _videoOutput.suppressesPlayerRendering = NO;
  [_item addOutput:_videoOutput];
  _legibleOutput = [[AVPlayerItemLegibleOutput alloc]
      initWithMediaSubtypesForNativeRepresentation:@[ @(kCMSubtitleFormatType_WebVTT) ]];
  _legibleOutput.suppressesPlayerRendering = YES;
  [_legibleOutput setDelegate:self queue:_queue];
  [_item addOutput:_legibleOutput];

  __weak SKMediaCore* weakSelf = self;
  AVPlayerItem* item = _item;
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [_observers addObject:[center addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                            object:item
                                             queue:_notifications
                                        usingBlock:^(NSNotification*) {
                                          SKMediaCore* strong = weakSelf;
                                          if (strong != nil && strong->_item == item) [strong reachedEnd];
                                        }]];
  [_observers addObject:[center addObserverForName:AVPlayerItemFailedToPlayToEndTimeNotification
                                            object:item
                                             queue:_notifications
                                        usingBlock:^(NSNotification* note) {
                                          SKMediaCore* strong = weakSelf;
                                          if (strong == nil || strong->_item != item) return;
                                          [strong failWithError:note.userInfo[AVPlayerItemFailedToPlayToEndTimeErrorKey]];
                                        }]];

  [_player replaceCurrentItemWithPlayerItem:_item];
  [_player pause];
  _loaded = true;

  // Variants and the media-selection groups load on their own; the tracks go
  // out once both they and the item are ready.
  AVURLAsset* asset = _asset;
  [asset loadValuesAsynchronouslyForKeys:@[ @"variants", @"availableMediaCharacteristicsWithMediaSelectionOptions",
                                            @"tracks", @"duration" ]
                       completionHandler:^{
                         SKMediaCore* strong = weakSelf;
                         if (strong == nil) return;
                         dispatch_async(strong->_queue, ^{
                           if (strong->_asset != asset || strong->_serial != serial) return;
                           [strong assetKeysLoaded];
                         });
                       }];
  [self startTimer:0.1];
  [self placePlane];
}

- (void)assetKeysLoaded {
  _assetKeysLoaded = true;
  NSError* error = nil;
  if ([_asset statusOfValueForKey:@"variants" error:&error] == AVKeyValueStatusLoaded) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    _variants = _asset.variants;
#pragma clang diagnostic pop
  }
  if ([_asset statusOfValueForKey:@"availableMediaCharacteristicsWithMediaSelectionOptions" error:&error] ==
      AVKeyValueStatusLoaded) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    _legibleGroup = [_asset mediaSelectionGroupForMediaCharacteristic:AVMediaCharacteristicLegible];
    _audibleGroup = [_asset mediaSelectionGroupForMediaCharacteristic:AVMediaCharacteristicAudible];
#pragma clang diagnostic pop
  }
  [self poll:true];
}

- (void)teardownItem {
  [self stopTimer];
  for (id observer in _observers) [[NSNotificationCenter defaultCenter] removeObserver:observer];
  [_observers removeAllObjects];
  for (auto& entry : _keyRequests) {
    [entry.second processContentKeyResponseError:[NSError errorWithDomain:@"dev.screenkit.media"
                                                                     code:1
                                                                 userInfo:@{NSLocalizedDescriptionKey : @"unloaded"}]];
  }
  _keyRequests.clear();
  if (_keySession != nil) {
    if (_asset != nil) [_keySession removeContentKeyRecipient:_asset];
    _keySession = nil;
  }
  if (_item != nil) {
    [_legibleOutput setDelegate:nil queue:nil];
    [_item removeOutput:_legibleOutput];
    [_item removeOutput:_videoOutput];
    [_item cancelPendingSeeks];
    [_asset cancelLoading];
    [_player pause];
    [_player replaceCurrentItemWithPlayerItem:nil];
  }
  _legibleOutput = nil;
  _videoOutput = nil;
  _item = nil;
  _asset = nil;
  _loaded = false;
  _seekGeneration++;
}

// ---- the poll ----------------------------------------------------------------------------

- (void)startTimer:(double)interval {
  if (_timer != nullptr && std::fabs(_timerInterval - interval) < 1e-6) return;
  [self stopTimer];
  _timerInterval = interval;
  _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
  const uint64_t nanos = static_cast<uint64_t>(interval * NSEC_PER_SEC);
  dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(nanos)), nanos, nanos / 10);
  __weak SKMediaCore* weakSelf = self;
  dispatch_source_set_event_handler(_timer, ^{
    SKMediaCore* strong = weakSelf;
    if (strong != nil && !strong->_stopped.load()) [strong poll:false];
  });
  dispatch_resume(_timer);
}

- (void)stopTimer {
  if (_timer == nullptr) return;
  dispatch_source_cancel(_timer);
  _timer = nullptr;
}

/// Polled fast while frames are coming -- so every new one the video output
/// vends is counted -- and at 10 Hz otherwise.
- (void)retime {
  if (!_loaded) return;
  double fps = 30;
  for (AVPlayerItemTrack* track in _item.tracks) {
    if ([track.assetTrack.mediaType isEqualToString:AVMediaTypeVideo] && track.currentVideoFrameRate > 0) {
      fps = track.currentVideoFrameRate;
    }
  }
  const bool frames = _player.timeControlStatus == AVPlayerTimeControlStatusPlaying;
  const double interval = frames ? std::max(1.0 / 120.0, std::min(1.0 / 60.0, 1.0 / (2.0 * fps))) : 0.1;
  [self startTimer:interval];
}

static double nowNs() { return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }

- (void)countFrames {
  if (_videoOutput == nil) return;
  const CMTime time = [_videoOutput itemTimeForHostTime:CACurrentMediaTime()];
  if (!CMTIME_IS_VALID(time)) return;
  if (![_videoOutput hasNewPixelBufferForItemTime:time]) return;
  CVPixelBufferRef buffer = [_videoOutput copyPixelBufferForItemTime:time itemTimeForDisplay:nullptr];
  if (buffer != nullptr) {
    _decodedFrames++;
    CVBufferRelease(buffer);
  }
}

- (void)poll:(bool)force {
  if (!_loaded || _item == nil) return;
  if (_item.status == AVPlayerItemStatusFailed) {
    [self failWithError:_item.error];
    return;
  }
  [self countFrames];
  const double now = nowNs();
  if (!force && now - _lastSlowNs < 95e6) return;
  _lastSlowNs = now;

  if (_item.status != AVPlayerItemStatusReadyToPlay) return;
  if (_readyAtNs == 0) _readyAtNs = now;
  if (!_metadataSent) {
    if (![self trySendMetadata:now]) return;
  }
  if (!_tracksSent && _assetKeysLoaded) {
    [self applyText];
    [self sendTracks];
  }
  [self updateMetadata];
  [self updateSize];
  [self updateVariant];
  [self updateState];
  [self sendTime:false];
  [self updateBuffered:now];
  if (now - _lastStatsNs >= 1e9) {
    _lastStatsNs = now;
    [self sendStats];
  }
  [self retime];
}

- (bool)hasVideo {
  for (AVPlayerItemTrack* track in _item.tracks) {
    if ([track.assetTrack.mediaType isEqualToString:AVMediaTypeVideo]) return true;
  }
  for (AVAssetVariant* variant in _variants) {
    if (variant.videoAttributes != nil) return true;
  }
  return false;
}

- (bool)hasAudio {
  for (AVPlayerItemTrack* track in _item.tracks) {
    if ([track.assetTrack.mediaType isEqualToString:AVMediaTypeAudio]) return true;
  }
  for (AVAssetVariant* variant in _variants) {
    if (variant.audioAttributes != nil) return true;
  }
  return _audibleGroup != nil;
}

- (void)seekableStart:(double&)start end:(double&)end {
  start = 0;
  end = 0;
  NSArray<NSValue*>* ranges = _item.seekableTimeRanges;
  if (ranges.count == 0) {
    const double duration = secondsOf(_item.duration);
    if (std::isfinite(duration)) end = duration;
    return;
  }
  const CMTimeRange first = ranges.firstObject.CMTimeRangeValue;
  const CMTimeRange last = ranges.lastObject.CMTimeRangeValue;
  start = secondsOf(first.start);
  end = secondsOf(CMTimeRangeGetEnd(last));
  if (!std::isfinite(start)) start = 0;
  if (!std::isfinite(end)) end = start;
}

/// Metadata once the size is in -- HTMLMediaElement promises videoWidth at
/// loadedmetadata -- or once it is clear there is no picture to wait for.
- (bool)trySendMetadata:(double)now {
  const CGSize size = _item.presentationSize;
  const bool video = [self hasVideo];
  const bool sized = size.width > 0 && size.height > 0;
  if (video && !sized && now - _readyAtNs < 2e9) return false;
  if (!video && !_assetKeysLoaded && now - _readyAtNs < 2e9) return false;
  _info.duration = secondsOf(_item.duration);
  _info.live = std::isinf(_info.duration);
  [self seekableStart:_info.seekStart end:_info.seekEnd];
  _info.width = static_cast<int>(size.width);
  _info.height = static_cast<int>(size.height);
  _info.hasVideo = video;
  _info.hasAudio = [self hasAudio];
  const std::string path = lower(utf8(_asset.URL.path));
  const std::string mime = lower(_request.mimeType);
  const bool hls = (path.size() > 5 && path.compare(path.size() - 5, 5, ".m3u8") == 0) ||
                   mime.find("mpegurl") != std::string::npos || _variants.count > 0;
  _info.manifest = hls ? "hls" : "progressive";
  _width = _info.width;
  _height = _info.height;
  _metadataSent = true;
  const std::uint32_t serial = _serial;
  const MediaInfo info = _info;
  [self emit:^(MediaSink& sink) {
    sink.onMetadata(serial, info);
    if (info.width > 0) sink.onSize(serial, info.width, info.height);
  }];

  // A start position, then play if play was asked for meanwhile; or a seek
  // asked for before there was anything to seek in.
  double start = std::isfinite(_pendingSeek) ? _pendingSeek : _request.startTime;
  const bool report = std::isfinite(_pendingSeek);
  _pendingSeek = kNaN;
  if (std::isfinite(start)) {
    _startPending = !report;
    [self seekInternal:start report:report];
  }
  [self applyRate];
  return true;
}

- (void)updateMetadata {
  const double duration = secondsOf(_item.duration);
  const bool live = std::isinf(duration);
  const bool changed = live != _info.live || (!live && std::isfinite(duration) && std::isfinite(_info.duration) &&
                                              std::fabs(duration - _info.duration) > 0.01) ||
                       (std::isnan(_info.duration) != std::isnan(duration));
  if (!changed) return;
  _info.duration = duration;
  _info.live = live;
  [self seekableStart:_info.seekStart end:_info.seekEnd];
  const std::uint32_t serial = _serial;
  const MediaInfo info = _info;
  [self emit:^(MediaSink& sink) { sink.onMetadata(serial, info); }];
}

- (void)updateSize {
  const CGSize size = _item.presentationSize;
  const int width = static_cast<int>(size.width);
  const int height = static_cast<int>(size.height);
  if (width <= 0 || height <= 0 || (width == _width && height == _height)) return;
  _width = width;
  _height = height;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onSize(serial, width, height); }];
}

- (void)updateState {
  PlaybackState state;
  if (_ended) {
    state = PlaybackState::Ended;
  } else if (_seeking || _startPending) {
    state = PlaybackState::Buffering;
  } else if (_player.timeControlStatus == AVPlayerTimeControlStatusPlaying) {
    state = PlaybackState::Ready;
  } else if (_player.timeControlStatus == AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate) {
    NSString* reason = _player.reasonForWaitingToPlay;
    state = [reason isEqualToString:AVPlayerWaitingWithNoItemToPlayReason] ? PlaybackState::Loading
                                                                          : PlaybackState::Buffering;
  } else {
    // Paused: ready when what is at the playhead is in.
    state = (_item.playbackLikelyToKeepUp || _item.playbackBufferFull || [self bufferedAhead:0.1])
                ? PlaybackState::Ready
                : PlaybackState::Buffering;
  }
  if (_stateSent && state == _state) return;
  _state = state;
  _stateSent = true;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onState(serial, state); }];
  [self sendTime:true];
}

- (bool)bufferedAhead:(double)seconds {
  const double position = secondsOf(_item.currentTime);
  for (NSValue* value in _item.loadedTimeRanges) {
    const CMTimeRange range = value.CMTimeRangeValue;
    const double start = secondsOf(range.start);
    const double end = secondsOf(CMTimeRangeGetEnd(range));
    if (position >= start - 0.05 && position + seconds <= end) return true;
  }
  return false;
}

- (void)sendTime:(bool)force {
  // Nothing until a start position has landed: the page reads the start
  // position meanwhile, and a report of 0 would move it back.
  if (!_metadataSent || _item == nil || _startPending) return;
  double position = secondsOf(_item.currentTime);
  if (!std::isfinite(position)) return;
  if (_ended && std::isfinite(_info.duration)) position = std::max(position, _info.duration);
  const double now = nowNs();
  double start = 0;
  double end = 0;
  [self seekableStart:start end:end];
  // A live window moves while playback is paused too, and the page reads it.
  const bool windowMoved = _info.live && (std::fabs(start - _lastWindowStart) > 1e-3 ||
                                          std::fabs(end - _lastWindowEnd) > 1e-3);
  const bool moved = !std::isfinite(_lastTimeSent) || std::fabs(position - _lastTimeSent) > 1e-3 || windowMoved;
  if (!force && (!moved || now - _lastTimeNs < 240e6)) return;
  _lastTimeSent = position;
  _lastTimeNs = now;
  _lastWindowStart = start;
  _lastWindowEnd = end;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onTime(serial, position, start, end); }];
}

- (void)updateBuffered:(double)now {
  if (now - _lastBufferedNs < 240e6) return;
  std::vector<TimeRange> ranges;
  for (NSValue* value in _item.loadedTimeRanges) {
    const CMTimeRange range = value.CMTimeRangeValue;
    TimeRange r;
    r.start = secondsOf(range.start);
    r.end = secondsOf(CMTimeRangeGetEnd(range));
    if (std::isfinite(r.start) && std::isfinite(r.end) && r.end > r.start) ranges.push_back(r);
  }
  std::sort(ranges.begin(), ranges.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
  bool same = ranges.size() == _buffered.size();
  for (size_t i = 0; same && i < ranges.size(); ++i) {
    same = std::fabs(ranges[i].start - _buffered[i].start) < 0.01 && std::fabs(ranges[i].end - _buffered[i].end) < 0.01;
  }
  if (same) return;
  _lastBufferedNs = now;
  _buffered = ranges;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onBuffered(serial, ranges); }];
}

- (void)reachedEnd {
  if (!_loaded || _ended) return;
  _ended = true;
  [self updateState];
  [self sendTime:true];
  [self sendStats];
}

// ---- seeking and rate -----------------------------------------------------------------------

- (void)seekInternal:(double)position report:(bool)report {
  if (!_loaded) return;
  if (!_metadataSent) {
    _pendingSeek = position;
    return;
  }
  const std::uint64_t generation = ++_seekGeneration;
  _seeking = true;
  _ended = false;
  const std::uint32_t serial = _serial;
  __weak SKMediaCore* weakSelf = self;
  const CMTime target = CMTimeMakeWithSeconds(position, 90000);
  [_item seekToTime:target
        toleranceBefore:kCMTimeZero
         toleranceAfter:kCMTimeZero
      completionHandler:^(BOOL) {
        SKMediaCore* strong = weakSelf;
        if (strong == nil) return;
        dispatch_async(strong->_queue, ^{
          if (strong->_stopped.load() || generation != strong->_seekGeneration || strong->_serial != serial ||
              !strong->_loaded) {
            return;
          }
          strong->_seeking = false;
          strong->_startPending = false;
          const double where = secondsOf(strong->_item.currentTime);
          if (report) {
            [strong emit:^(MediaSink& sink) { sink.onSeeked(serial, where); }];
          }
          [strong applyRate];
          [strong updateState];
          [strong sendTime:true];
        });
      }];
  [self updateState];
}

- (void)applyRate {
  if (!_loaded) return;
  if (_wantsPlay && !_startPending && !_ended) {
    if (_player.rate != static_cast<float>(_rate)) _player.rate = static_cast<float>(_rate);
  } else if (_player.rate != 0) {
    [_player pause];
  }
  [self retime];
}

// ---- tracks ------------------------------------------------------------------------------------

- (void)applyAbr {
  if (_item == nil) return;
  double peak = 0;
  CGSize maxSize = CGSizeZero;
  if (_pinnedVariant >= 0 && _pinnedVariant < static_cast<int>(_variants.count)) {
    // AVPlayer has no way to pin a variant: capping the bit rate and the
    // resolution at this one's is the closest, and it may still go lower
    // (runtime/js/README.md, "Where the platforms differ").
    AVAssetVariant* variant = _variants[_pinnedVariant];
    peak = variant.peakBitRate > 0 ? variant.peakBitRate : variant.averageBitRate;
    if (variant.videoAttributes != nil) maxSize = variant.videoAttributes.presentationSize;
  } else {
    if (std::isfinite(_abr.maxBandwidth) && _abr.maxBandwidth > 0) peak = _abr.maxBandwidth;
    const bool widthCapped = _abr.maxWidth < std::numeric_limits<int>::max();
    const bool heightCapped = _abr.maxHeight < std::numeric_limits<int>::max();
    if (widthCapped || heightCapped) {
      maxSize = CGSizeMake(widthCapped ? _abr.maxWidth : 100000, heightCapped ? _abr.maxHeight : 100000);
    }
  }
  _item.preferredPeakBitRate = peak;
  _item.preferredMaximumResolution = maxSize;
}

- (int)variantForBitrate:(double)bitrate {
  if (!(bitrate > 0)) return -1;
  int best = -1;
  double bestDistance = kInf;
  for (NSUInteger i = 0; i < _variants.count; ++i) {
    AVAssetVariant* variant = _variants[i];
    for (double rate : {variant.peakBitRate, variant.averageBitRate}) {
      if (!(rate > 0)) continue;
      const double distance = std::fabs(rate - bitrate);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = static_cast<int>(i);
      }
    }
  }
  return best >= 0 && bestDistance <= bitrate * 0.02 + 1 ? best : -1;
}

- (void)updateVariant {
  int active = -1;
  if (_variants.count > 0) {
    AVPlayerItemAccessLogEvent* event = _item.accessLog.events.lastObject;
    // The variant playing is the one whose BANDWIDTH the access log indicates;
    // before the first segment there is no answer, and none is guessed.
    if (event != nil) active = [self variantForBitrate:event.indicatedBitrate];
  } else if (_metadataSent) {
    active = 0;  // progressive: the one variant
  }
  if (active < 0 || active == _activeVariant) return;
  _activeVariant = active;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onVariantChanged(serial, active); }];
  if (_tracksSent) [self sendTracks];
}

- (void)sendTracks {
  TrackList tracks;
  AVMediaSelection* selection = _item.currentMediaSelection;
  AVMediaSelectionOption* audible = _audibleGroup ? [selection selectedMediaOptionInMediaSelectionGroup:_audibleGroup] : nil;
  AVMediaSelectionOption* legible = _legibleGroup ? [selection selectedMediaOptionInMediaSelectionGroup:_legibleGroup] : nil;

  const std::string audioLanguage = audible != nil ? utf8(audible.extendedLanguageTag ?: [audible.locale objectForKey:NSLocaleLanguageCode])
                                                   : std::string();
  if (_variants.count > 0) {
    for (NSUInteger i = 0; i < _variants.count; ++i) {
      AVAssetVariant* variant = _variants[i];
      VariantTrack v;
      v.id = static_cast<int>(i);
      v.bandwidth = variant.peakBitRate > 0 ? variant.peakBitRate : variant.averageBitRate;
      if (variant.videoAttributes != nil) {
        v.width = static_cast<int>(variant.videoAttributes.presentationSize.width);
        v.height = static_cast<int>(variant.videoAttributes.presentationSize.height);
        v.frameRate = variant.videoAttributes.nominalFrameRate;
        NSNumber* codec = variant.videoAttributes.codecTypes.firstObject;
        if (codec != nil) v.videoCodec = fourCC(codec.unsignedIntValue);
      }
      if (variant.audioAttributes != nil) {
        NSNumber* format = variant.audioAttributes.formatIDs.firstObject;
        if (format != nil) v.audioCodec = audioCodecName(format.unsignedIntValue);
        if (audible != nil) {
          AVAssetVariantAudioRenditionSpecificAttributes* rendition =
              [variant.audioAttributes renditionSpecificAttributesForMediaOption:audible];
          if (rendition != nil) v.channels = static_cast<int>(rendition.channelCount);
        }
      }
      v.language = audioLanguage;
      v.audioId = audible != nil && _audibleGroup != nil
                      ? static_cast<int>([_audibleGroup.options indexOfObject:audible])
                      : -1;
      v.active = static_cast<int>(i) == _activeVariant;
      tracks.variants.push_back(v);
    }
  } else {
    // Progressive: one variant, from the file's own tracks.
    VariantTrack v;
    v.id = 0;
    v.active = true;
    v.language = audioLanguage;
    double bandwidth = 0;
    for (AVAssetTrack* track in _asset.tracks) {
      bandwidth += track.estimatedDataRate;
      CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
      if ([track.mediaType isEqualToString:AVMediaTypeVideo]) {
        v.width = static_cast<int>(std::fabs(track.naturalSize.width));
        v.height = static_cast<int>(std::fabs(track.naturalSize.height));
        v.frameRate = track.nominalFrameRate;
        if (format != nullptr) v.videoCodec = fourCC(CMFormatDescriptionGetMediaSubType(format));
      } else if ([track.mediaType isEqualToString:AVMediaTypeAudio]) {
        if (format != nullptr) {
          v.audioCodec = audioCodecName(CMFormatDescriptionGetMediaSubType(format));
          const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format);
          if (asbd != nullptr) v.channels = static_cast<int>(asbd->mChannelsPerFrame);
        }
      }
    }
    v.bandwidth = bandwidth;
    tracks.variants.push_back(v);
  }

  if (_audibleGroup != nil) {
    for (NSUInteger i = 0; i < _audibleGroup.options.count; ++i) {
      AVMediaSelectionOption* option = _audibleGroup.options[i];
      AudioTrack a;
      a.id = static_cast<int>(i);
      a.language = utf8(option.extendedLanguageTag ?: [option.locale objectForKey:NSLocaleLanguageCode]);
      a.label = utf8(option.displayName);
      a.role = [option hasMediaCharacteristic:AVMediaCharacteristicIsMainProgramContent] ? "main" : "";
      a.active = option == audible;
      tracks.audio.push_back(a);
    }
  } else if ([self hasAudio]) {
    // Audio muxed with the video, in no group: the one track there is.
    AudioTrack a;
    a.id = 0;
    a.active = true;
    if (!tracks.variants.empty()) {
      a.codec = tracks.variants.front().audioCodec;
      a.channels = tracks.variants.front().channels;
    }
    tracks.audio.push_back(a);
  }

  if (_legibleGroup != nil) {
    for (NSUInteger i = 0; i < _legibleGroup.options.count; ++i) {
      AVMediaSelectionOption* option = _legibleGroup.options[i];
      TextTrack t;
      t.id = static_cast<int>(i);
      t.language = utf8(option.extendedLanguageTag ?: [option.locale objectForKey:NSLocaleLanguageCode]);
      t.label = utf8(option.displayName);
      t.kind = [option hasMediaCharacteristic:AVMediaCharacteristicTranscribesSpokenDialogForAccessibility]
                   ? "captions"
                   : "subtitles";
      if ([option.mediaType isEqualToString:AVMediaTypeClosedCaption]) {
        t.mimeType = "application/cea-608";
      } else {
        t.mimeType = "text/vtt";
      }
      t.forced = [option hasMediaCharacteristic:AVMediaCharacteristicContainsOnlyForcedSubtitles];
      t.active = option == legible;
      tracks.text.push_back(t);
    }
  }
  _tracksSent = true;
  const std::uint32_t serial = _serial;
  auto shared = std::make_shared<TrackList>(std::move(tracks));
  [self emit:^(MediaSink& sink) { sink.onTracks(serial, *shared); }];
}

- (void)applyText {
  if (_item == nil || _legibleGroup == nil) return;
  AVMediaSelectionOption* option = nil;
  if (_selectedText >= 0 && _selectedText < static_cast<int>(_legibleGroup.options.count)) {
    option = _legibleGroup.options[_selectedText];
  } else if (_selectedText == -1 && !_tracksSent && !_request.textLanguage.empty()) {
    // The load's preferred text language picks the first track in it.
    for (NSUInteger i = 0; i < _legibleGroup.options.count; ++i) {
      AVMediaSelectionOption* candidate = _legibleGroup.options[i];
      const std::string language = lower(utf8(candidate.extendedLanguageTag ?: [candidate.locale objectForKey:NSLocaleLanguageCode]));
      if (language.rfind(lower(_request.textLanguage), 0) == 0) {
        option = candidate;
        _selectedText = static_cast<int>(i);
        break;
      }
    }
  }
  [_item selectMediaOption:option inMediaSelectionGroup:_legibleGroup];
  if (option == nil && !_cues.empty()) {
    _cues.clear();
  }
  if (_tracksSent) [self sendTracks];
}

- (void)applyAudioLanguage:(const std::string&)language role:(const std::string&)role {
  if (_item == nil || _audibleGroup == nil) return;
  const std::string wanted = lower(language);
  AVMediaSelectionOption* chosen = nil;
  for (AVMediaSelectionOption* option in _audibleGroup.options) {
    const std::string tag = lower(utf8(option.extendedLanguageTag ?: [option.locale objectForKey:NSLocaleLanguageCode]));
    if (!wanted.empty() && tag.rfind(wanted, 0) != 0) continue;
    if (!role.empty() && role == "main" && ![option hasMediaCharacteristic:AVMediaCharacteristicIsMainProgramContent]) {
      continue;
    }
    chosen = option;
    break;
  }
  if (chosen == nil) return;
  [_item selectMediaOption:chosen inMediaSelectionGroup:_audibleGroup];
  if (_tracksSent) [self sendTracks];
}

// ---- cues: AVPlayerItemLegibleOutputPushDelegate, on the queue --------------------------------

- (void)legibleOutput:(AVPlayerItemLegibleOutput*)output
    didOutputAttributedStrings:(NSArray<NSAttributedString*>*)strings
           nativeSampleBuffers:(NSArray*)nativeSamples
                   forItemTime:(CMTime)itemTime {
  if (output != _legibleOutput || !_loaded || _selectedText < 0) return;
  std::vector<Cue> cues;
  const double at = secondsOf(itemTime);
  for (id sample in nativeSamples) {
    CMSampleBufferRef buffer = (__bridge CMSampleBufferRef)sample;
    const double start = secondsOf(CMSampleBufferGetPresentationTimeStamp(buffer));
    const double duration = secondsOf(CMSampleBufferGetDuration(buffer));
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buffer);
    if (block == nullptr) continue;
    const size_t length = CMBlockBufferGetDataLength(block);
    std::vector<uint8_t> bytes(length);
    if (length == 0 || CMBlockBufferCopyDataBytes(block, 0, length, bytes.data()) != kCMBlockBufferNoErr) continue;
    std::vector<std::string> texts;
    vttPayloads(bytes.data(), bytes.size(), texts);
    for (auto& text : texts) {
      // The sample's own timestamp is on the stream's clock -- an HLS WebVTT
      // cue keeps its MPEG-TS offset, 1.4 s in ffmpeg's segments -- and the item
      // time it is delivered for is the playback clock. The offset between the
      // two is what a cue delivered at its own start shows; one delivered
      // mid-cue, after a seek, shows less, so the largest seen is kept.
      if (std::isfinite(start) && std::isfinite(at)) {
        const double offset = start - at;
        if (!std::isfinite(_cueOffset) || offset > _cueOffset) _cueOffset = offset;
      }
      Cue cue;
      cue.start = std::isfinite(start) && std::isfinite(_cueOffset) ? std::max(0.0, start - _cueOffset) : at;
      cue.end = std::isfinite(duration) && duration > 0 ? cue.start + duration : cue.start;
      cue.text = std::move(text);
      cues.push_back(std::move(cue));
    }
  }
  for (NSAttributedString* string in strings) {
    // Anything not WebVTT (CEA-608, TTML) arrives styled: its text, and no end
    // time AVFoundation will tell -- the next delivery replaces it.
    Cue cue;
    cue.start = at;
    cue.end = at;
    cue.text = utf8(string.string);
    cues.push_back(std::move(cue));
  }
  bool same = cues.size() == _cues.size();
  for (size_t i = 0; same && i < cues.size(); ++i) same = cues[i].text == _cues[i].text && cues[i].start == _cues[i].start;
  if (same) return;
  _cues = cues;
  const std::uint32_t serial = _serial;
  const int track = _selectedText;
  auto shared = std::make_shared<std::vector<Cue>>(std::move(cues));
  [self emit:^(MediaSink& sink) { sink.onCues(serial, track, *shared); }];
}

// ---- stats ----------------------------------------------------------------------------------------

- (void)sendStats {
  if (_item == nil) return;
  MediaStats stats;
  const CGSize size = _item.presentationSize;
  stats.width = static_cast<int>(size.width);
  stats.height = static_cast<int>(size.height);
  for (AVPlayerItemTrack* track in _item.tracks) {
    if ([track.assetTrack.mediaType isEqualToString:AVMediaTypeVideo] && track.currentVideoFrameRate > 0) {
      stats.frameRate = track.currentVideoFrameRate;
    }
  }
  std::int64_t dropped = 0;
  AVPlayerItemAccessLogEvent* last = nil;
  for (AVPlayerItemAccessLogEvent* event in _item.accessLog.events) {
    if (event.numberOfDroppedVideoFrames > 0) dropped += event.numberOfDroppedVideoFrames;
    last = event;
  }
  if (last != nil) {
    if (last.indicatedBitrate > 0) stats.streamBandwidth = last.indicatedBitrate;
    if (last.observedBitrate > 0) stats.estimatedBandwidth = last.observedBitrate;
  }
  if (std::isnan(stats.streamBandwidth) && _activeVariant >= 0 && _activeVariant < static_cast<int>(_variants.count)) {
    stats.streamBandwidth = [_variants[_activeVariant] peakBitRate];
  }
  stats.decodedFrames = _decodedFrames;
  stats.droppedFrames = static_cast<std::uint64_t>(dropped);
  stats.corruptedFrames = 0;
  const std::uint32_t serial = _serial;
  [self emit:^(MediaSink& sink) { sink.onStats(serial, stats); }];
}

// ---- FairPlay: AVContentKeySessionDelegate, on the queue ------------------------------------------

- (void)contentKeySession:(AVContentKeySession*)session didProvideContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (session != _keySession) return;
  [self handleKeyRequest:keyRequest];
}

- (void)contentKeySession:(AVContentKeySession*)session
    didProvideRenewingContentKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (session != _keySession) return;
  [self handleKeyRequest:keyRequest];
}

- (void)contentKeySession:(AVContentKeySession*)session
       contentKeyRequest:(AVContentKeyRequest*)keyRequest
        didFailWithError:(NSError*)error {
  (void)keyRequest;
  if (session != _keySession || !_loaded) return;
  [self fail:MediaErrorKind::Licence status:0 message:"FairPlay refused the key: " + describe(error)];
}

- (void)handleKeyRequest:(AVContentKeyRequest*)keyRequest {
  if (!_loaded) return;
  _keyRequested = true;
  NSString* identifier = [keyRequest.identifier isKindOfClass:[NSString class]] ? keyRequest.identifier : @"";
  auto certificate = _request.drm.serverCertificate;
  if (!certificate || certificate->empty()) {
    [keyRequest processContentKeyResponseError:[NSError errorWithDomain:@"dev.screenkit.media"
                                                                   code:2
                                                               userInfo:@{NSLocalizedDescriptionKey : @"no certificate"}]];
    [self fail:MediaErrorKind::Licence
          status:0
         message:"FairPlay needs the application certificate: drm.advanced['com.apple.fps'].serverCertificate or "
                 "serverCertificateUri"];
    return;
  }
  NSData* appCertificate = [NSData dataWithBytes:certificate->data() length:certificate->size()];
  // The asset id the key server knows the content by: what follows skd://.
  NSString* assetId = [identifier hasPrefix:@"skd://"] ? [identifier substringFromIndex:6] : identifier;
  NSData* contentId = [assetId dataUsingEncoding:NSUTF8StringEncoding];
  const std::uint32_t serial = _serial;
  const std::string contentIdentifier = utf8(identifier);
  __weak SKMediaCore* weakSelf = self;
  [keyRequest makeStreamingContentKeyRequestDataForApp:appCertificate
                                     contentIdentifier:contentId
                                               options:@{AVContentKeyRequestProtocolVersionsKey : @[ @1 ]}
                                     completionHandler:^(NSData* spc, NSError* error) {
                                       SKMediaCore* strong = weakSelf;
                                       if (strong == nil) return;
                                       dispatch_async(strong->_queue, ^{
                                         if (strong->_stopped.load() || strong->_serial != serial || !strong->_loaded) {
                                           return;
                                         }
                                         if (spc == nil) {
                                           [keyRequest processContentKeyResponseError:error];
                                           [strong fail:MediaErrorKind::Licence
                                                 status:0
                                                message:"FairPlay could not make the key request (SPC) for " +
                                                        contentIdentifier + ": " + describe(error)];
                                           return;
                                         }
                                         const std::uint64_t requestId = strong->_nextRequestId++;
                                         strong->_keyRequests[requestId] = keyRequest;
                                         Bytes challenge(static_cast<const uint8_t*>(spc.bytes),
                                                         static_cast<const uint8_t*>(spc.bytes) + spc.length);
                                         auto shared = std::make_shared<Bytes>(std::move(challenge));
                                         [strong emit:^(MediaSink& sink) {
                                           sink.onLicenceRequest(serial, requestId, "com.apple.fps", *shared,
                                                                 contentIdentifier);
                                         }];
                                         // An answer that never comes is a
                                         // failure, not a stream that buffers for
                                         // ever: the same 30 s Android gives a
                                         // licence request (VideoPlayer.java).
                                         __weak SKMediaCore* weakAgain = strong;
                                         dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                                                      (int64_t)(kLicenceTimeoutSeconds * NSEC_PER_SEC)),
                                                        strong->_queue, ^{
                                           SKMediaCore* alive = weakAgain;
                                           if (alive == nil || alive->_stopped.load() || alive->_serial != serial) {
                                             return;
                                           }
                                           if (alive->_keyRequests.find(requestId) == alive->_keyRequests.end()) return;
                                           [alive answerKeyRequest:requestId licence:nullptr];
                                         });
                                       });
                                     }];
}

- (void)answerKeyRequest:(uint64_t)requestId licence:(std::shared_ptr<const Bytes>)licence {
  auto it = _keyRequests.find(requestId);
  if (it == _keyRequests.end()) return;
  AVContentKeyRequest* keyRequest = it->second;
  _keyRequests.erase(it);
  if (!licence || licence->empty()) {
    [keyRequest processContentKeyResponseError:[NSError errorWithDomain:@"dev.screenkit.media"
                                                                   code:3
                                                               userInfo:@{NSLocalizedDescriptionKey : @"licence request failed"}]];
    [self fail:MediaErrorKind::Licence status:0 message:"the licence request failed"];
    return;
  }
  NSData* ckc = [NSData dataWithBytes:licence->data() length:licence->size()];
  [keyRequest processContentKeyResponse:[AVContentKeyResponse contentKeyResponseWithFairPlayStreamingKeyResponseData:ckc]];
}

// ---- the plane ------------------------------------------------------------------------------------

- (void)placePlane {
  if (!_planeSet) return;
  const VideoHost host = videoHost();
  if (host.view == nullptr) return;  // headless: the plane goes nowhere, the video still plays
  if (_plane == nil) _plane = [[SKPlaneHost alloc] initWithPlayer:_player];
  SKPlaneHost* plane = _plane;
  const PlaneRect rect = planeInWindow(_planeRect, 0, 0, host.drawableWidth, host.drawableHeight);
  const BOOL visible = _planeVisible;
  void* view = host.view;
  dispatch_async(dispatch_get_main_queue(), ^{ [plane applyRect:rect visible:visible view:view]; });
}

@end

// ---- the seam ----------------------------------------------------------------------------------

namespace screenkit::media {
namespace {

class AppleMediaPlayer final : public MediaPlayer {
 public:
  explicit AppleMediaPlayer(MediaConfig config)
      : core_([[SKMediaCore alloc] initWithName:config.name sink:std::move(config.sink)]) {}
  ~AppleMediaPlayer() override { shutdown(); }

  void load(LoadRequest request) override { [core_ load:std::make_shared<LoadRequest>(std::move(request))]; }
  void play() override { [core_ play]; }
  void pause() override { [core_ pause]; }
  void seek(double position) override { [core_ seek:position]; }
  void setRate(double rate) override { [core_ setRate:rate]; }
  void setVolume(double volume) override { [core_ setVolume:volume]; }
  void setMuted(bool muted) override { [core_ setMuted:muted]; }
  void setPlane(PlaneRect rect, bool visible) override { [core_ setPlane:rect visible:visible]; }
  void selectVariant(int variantId) override { [core_ selectVariant:variantId]; }
  void setAbr(AbrConfig abr) override { [core_ setAbr:abr]; }
  void selectAudioLanguage(std::string language, std::string role) override {
    [core_ selectAudioLanguage:std::move(language) role:std::move(role)];
  }
  void selectText(int textTrackId) override { [core_ selectText:textTrackId]; }
  void provideLicence(std::uint64_t requestId, std::shared_ptr<const Bytes> licence) override {
    [core_ provideLicence:requestId licence:std::move(licence)];
  }
  void unload() override { [core_ unload]; }
  void shutdown() override { [core_ shutdown]; }

 private:
  SKMediaCore* core_;
};

bool playable(NSString* type) { return [AVURLAsset isPlayableExtendedMIMEType:type]; }

}  // namespace

std::shared_ptr<MediaPlayer> MediaPlayer::create(MediaConfig config) {
  return std::make_shared<AppleMediaPlayer>(std::move(config));
}

MediaCapabilities mediaCapabilities() {
  static const MediaCapabilities caps = [] {
    MediaCapabilities out;
    out.available = true;
    out.platform = "apple";
    out.hls = true;
    out.progressive = true;
    out.dash = false;
#if !TARGET_OS_SIMULATOR
    out.keySystems = {"com.apple.fps"};  // a simulator has no FairPlay
#endif
    out.videoOutput = true;
    for (NSString* container in @[ @"video/mp4", @"audio/mp4", @"video/quicktime", @"video/x-m4v",
                                   @"application/vnd.apple.mpegurl", @"application/x-mpegurl", @"audio/mpegurl" ]) {
      if (playable(container)) out.containers.push_back(utf8(container));
    }
    // Asked of AVFoundation, one representative codecs string per prefix.
    const std::pair<const char*, NSString*> codecs[] = {
        {"avc1", @"video/mp4; codecs=\"avc1.64001f\""},  {"avc3", @"video/mp4; codecs=\"avc3.64001f\""},
        {"hvc1", @"video/mp4; codecs=\"hvc1.1.6.L93.B0\""}, {"hev1", @"video/mp4; codecs=\"hev1.1.6.L93.B0\""},
        {"dvh1", @"video/mp4; codecs=\"dvh1.05.06\""},    {"av01", @"video/mp4; codecs=\"av01.0.05M.08\""},
        {"vp09", @"video/mp4; codecs=\"vp09.00.10.08\""}, {"mp4a", @"audio/mp4; codecs=\"mp4a.40.2\""},
        {"ac-3", @"audio/mp4; codecs=\"ac-3\""},          {"ec-3", @"audio/mp4; codecs=\"ec-3\""},
        {"opus", @"audio/mp4; codecs=\"opus\""},          {"flac", @"audio/mp4; codecs=\"flac\""},
        {"alac", @"audio/mp4; codecs=\"alac\""},
    };
    for (const auto& codec : codecs) {
      if (playable(codec.second)) out.codecs.emplace_back(codec.first);
    }
    return out;
  }();
  return caps;
}

bool mediaAvailable() { return true; }

}  // namespace screenkit::media
