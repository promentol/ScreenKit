// Copyright (c) ScreenKit contributors. MIT.
//
// Apple's network client: NSURLSession, exactly as React Native's
// `RCTHTTPRequestHandler` uses it, behind the `NetService` seam.
//
// There is no protocol code here. NSURLSession resolves names, opens and pools
// connections, speaks HTTP/1.1 and HTTP/2, follows redirects, decodes
// `Content-Encoding`, verifies certificates against the system trust store and
// keeps cookies in `NSHTTPCookieStorage` -- all of it maintained by the OS. What
// this file does is translate: a `HttpRequestSpec` into an `NSURLRequest`, the
// delegate callbacks into `HttpSink` calls, and `NSError` into `NetError`.
//
// Threading. One serial dispatch queue per runtime is the session's delegate
// queue, so every delegate callback and every call this file makes into its own
// maps happens on it, in order, with no locks. The public methods are called
// from the JS thread and do nothing but `dispatch_async` onto that queue;
// `shutdown` is the one exception and uses `dispatch_sync`, which is what makes
// "nothing is delivered after it returns" true.
//
// Sink calls do **not** run on that queue. Each one is handed, in the order it
// was made, to a second serial queue (`AppleCore::delivery`, through
// `deliver`), and the delegate returns at once. That is what makes backpressure
// hold: `[task suspend]` only stops CFNetwork reading the socket if the delegate
// is caught up when it is called. Measured against a 64 MiB body with the task
// suspended at 1 MiB -- a delegate that spends 20 ms per callback copying bytes
// sees `suspend` ignored and all 64 MiB read into memory, at loopback speed and
// at 128 MB/s alike; a delegate that only counts, suspends and hands the NSData
// on holds the server at about 3 MiB even when whatever consumes it takes 100 ms
// a chunk. So the copy into `Bytes`, and every other sink call with it, happens
// over there.
//
// Built with ARC.
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NetService.h"

namespace screenkit::net {
namespace {

/// Browsers show a status code's canonical reason phrase, and so does
/// NSURLSession: `allHeaderFields` has no status line in it, and there is no
/// public way to read what the server actually wrote. RN has the same
/// limitation. This is that table (RFC 9110 and the IANA registry).
const char* canonicalStatusText(int status) {
  switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return "";
  }
}

/// `NSURLError*` folded into the ten kinds. `receivedResponse` separates a
/// connection that never came up from one that broke mid-exchange, as the
/// Network.framework backend's `classify()` did with `ready`.
NetError classifyError(NSError* error, bool receivedResponse) {
  if (error == nil) return NetError::None;
  if (![error.domain isEqualToString:NSURLErrorDomain]) {
    return receivedResponse ? NetError::Network : NetError::Connect;
  }
  switch (error.code) {
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed:
      return NetError::Dns;
    case NSURLErrorCannotConnectToHost:
    case NSURLErrorTimedOut:
    case NSURLErrorNotConnectedToInternet:
    case NSURLErrorInternationalRoamingOff:
    case NSURLErrorCallIsActive:
    case NSURLErrorDataNotAllowed:
      return NetError::Connect;
    case NSURLErrorSecureConnectionFailed:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired:
    case NSURLErrorAppTransportSecurityRequiresSecureConnection:
      return NetError::Tls;
    case NSURLErrorBadServerResponse:
    case NSURLErrorCannotParseResponse:
    case NSURLErrorZeroByteResource:
      return NetError::Protocol;
    case NSURLErrorCannotDecodeRawData:
    case NSURLErrorCannotDecodeContentData:
      return NetError::Decode;
    case NSURLErrorHTTPTooManyRedirects:
    case NSURLErrorRedirectToNonExistentLocation:
      return NetError::Redirect;
    case NSURLErrorBadURL:
    case NSURLErrorUnsupportedURL:
    case NSURLErrorFileDoesNotExist:
      return NetError::Url;
    case NSURLErrorNetworkConnectionLost:
      return NetError::Network;
    default:
      return receivedResponse ? NetError::Network : NetError::Connect;
  }
}

std::string describe(NSError* error) {
  if (error == nil) return "the request failed";
  NSString* text = error.localizedDescription;
  if (text.length == 0) text = error.domain;
  return text != nil ? std::string(text.UTF8String) : std::string("the request failed");
}

/// `@("...")` is `stringWithUTF8String:`, which answers **nil** for bytes that
/// are not valid UTF-8 -- and a JS string with a lone surrogate reaches here as
/// a URL, a header, a cookie or a subprotocol. A nil then raises out of
/// `@{key: nil}`, `URLWithString:` and `forHTTPHeaderField:`, which would take
/// the process with it. Every string that came from JS is built through this,
/// and its caller fails the call instead.
NSString* toNSString(const std::string& text) {
  return [[NSString alloc] initWithBytes:text.data()
                                  length:text.size()
                                encoding:NSUTF8StringEncoding];
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

/// `NSHTTPURLResponse.allHeaderFields` as a `HeaderList`.
///
/// Two things are lost here and cannot be recovered through any public API, so
/// they are recorded rather than worked around (spec-platform-http-clients.md):
/// duplicate header lines -- several `Set-Cookie` among them -- arrive already
/// comma-joined into one value, and the dictionary has no order. Names are
/// lower-cased and the list is sorted by name, so at least the order is the same
/// from one run to the next.
HeaderList headersOf(NSHTTPURLResponse* response) {
  HeaderList out;
  NSDictionary* fields = response.allHeaderFields;
  out.reserve(fields.count);
  for (id key in fields) {
    if (![key isKindOfClass:[NSString class]]) continue;
    id value = fields[key];
    if (![value isKindOfClass:[NSString class]]) continue;
    out.emplace_back(lower(std::string([(NSString*)key UTF8String])),
                     std::string([(NSString*)value UTF8String]));
  }
  std::sort(out.begin(), out.end(),
            [](const std::pair<std::string, std::string>& a,
               const std::pair<std::string, std::string>& b) { return a.first < b.first; });
  return out;
}

/// The test suite's trust path, and only ever that: production sets no anchors,
/// so the challenge is answered with `PerformDefaultHandling` and the system
/// evaluates the chain against its own store. With anchors this does the same
/// evaluation -- chain, validity and host name -- with the extra roots added.
bool evaluateWithAnchors(SecTrustRef trust, const std::vector<Bytes>& anchors, NSString* host) {
  if (trust == nullptr) return false;
  NSMutableArray* certificates = [NSMutableArray array];
  for (const Bytes& der : anchors) {
    NSData* data = [NSData dataWithBytes:der.data() length:der.size()];
    SecCertificateRef certificate =
        SecCertificateCreateWithData(kCFAllocatorDefault, (__bridge CFDataRef)data);
    if (certificate != nullptr) [certificates addObject:(__bridge_transfer id)certificate];
  }
  // Only ever called with anchors to add. None of them parsing would otherwise
  // evaluate the chain against the system store in silence -- trusting a
  // different root than the one that was configured.
  if (certificates.count == 0) return false;
  SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef)certificates);
  SecTrustSetAnchorCertificatesOnly(trust, false);
  SecPolicyRef policy = SecPolicyCreateSSL(true, (__bridge CFStringRef)host);
  SecTrustSetPolicies(trust, policy);
  CFRelease(policy);
  CFErrorRef error = nullptr;
  const bool ok = SecTrustEvaluateWithError(trust, &error);
  if (error != nullptr) CFRelease(error);
  return ok;
}

struct AppleRequest;
struct AppleSocket;

/// Everything one runtime's client owns. Held by shared_ptr so the session
/// delegate, which outlives the service until `invalidateAndCancel` completes,
/// can never touch freed memory.
struct AppleCore {
  dispatch_queue_t queue = nil;
  /// Where sink calls run: see the file comment. Serial, and fed only from
  /// `queue`, so one request's events arrive in the order `queue` made them.
  dispatch_queue_t delivery = nil;
  /// Set by `shutdown` before it drains `delivery`: what is still queued there
  /// is dropped rather than delivered to a runtime that is going away.
  std::atomic<bool> silenced{false};
  NSURLSession* session = nil;
  NSHTTPCookieStorage* cookies = nil;
  std::vector<Bytes> testAnchors;
  bool stopped = false;

  std::unordered_map<std::uint64_t, std::shared_ptr<AppleRequest>> requests;
  std::unordered_map<NSUInteger, std::shared_ptr<AppleRequest>> requestsByTask;
  std::unordered_map<std::uint64_t, std::shared_ptr<AppleSocket>> sockets;
  std::unordered_map<NSUInteger, std::shared_ptr<AppleSocket>> socketsByTask;
};

/// Marks `AppleCore::delivery`, so `shutdown` can tell it is being run from a
/// sink call there -- where draining that queue synchronously would deadlock.
const void* const kDeliveryQueueKey = &kDeliveryQueueKey;

/// Hand one sink call to the delivery queue. Called on `core->queue` only, so
/// the calls reach the sink in the order they were made. `work` must capture
/// what it uses by value -- the sink among it -- because the request that made
/// the call may be retired before the call runs.
template <typename Work>
void deliver(const std::shared_ptr<AppleCore>& core, Work work) {
  std::shared_ptr<AppleCore> held = core;
  dispatch_async(core->delivery, ^{
    if (held->silenced.load()) return;
    @autoreleasepool {
      work();
    }
  });
}

/// `NSData` as `Bytes`. A body chunk may be discontiguous, which is why this
/// walks the ranges rather than reading `bytes`.
Bytes bytesOf(NSData* data) {
  __block Bytes out;
  out.reserve(data.length);
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
    (void)stop;
    const auto* start = static_cast<const std::uint8_t*>(bytes);
    out.insert(out.end(), start, start + range.length);
  }];
  return out;
}

}  // namespace
}  // namespace screenkit::net

// ---- the streamed request body -------------------------------------------------
//
// A `ReadableStream` request body arrives chunk by chunk from JS and has to
// reach NSURLSession as an NSInputStream. `CFStreamCreateBoundPair` gives the
// pair; the write end needs a run loop to tell it when the reader has made
// space, so one thread runs one for every upload in the process. Writing from
// the net queue instead and polling for space would burn the CPU exactly while
// a slow server holds the upload back, which is when it matters least.

@interface SKUploadPump : NSObject <NSStreamDelegate>
- (instancetype)init;
@property(nonatomic, readonly) NSInputStream* input;
/// Runs once, on the stream thread, when the last body byte has been handed to
/// the network -- which is the only thing that tells a streamed upload it is
/// complete, since its length is unknown and `didSendBodyData` never says so.
/// Not called on the abort path.
@property(nonatomic, copy) void (^onDrained)(void);
/// Any thread. False when the queue is full: the reader has stopped taking
/// bytes and the caller must not grow this without bound.
- (BOOL)append:(NSData*)data;
- (void)finish;
/// Any thread. Drops what is left and closes: the request is going away.
- (void)abort;
@end

@implementation SKUploadPump {
  NSOutputStream* _output;
  NSMutableArray<NSData*>* _pending;
  NSUInteger _offset;
  NSUInteger _pendingBytes;
  BOOL _ended;
  BOOL _closed;
  BOOL _opened;
  BOOL _drained;
  NSRecursiveLock* _lock;
}

/// What may sit between JS and a stalled server. There is no way to push back on
/// the page -- `__screenkit.net.write` is fire-and-forget -- so the choice is
/// between a bound and an unbounded queue, and a failed upload beats an OOM.
static const NSUInteger kMaxPendingBytes = 8 * 1024 * 1024;

+ (NSThread*)thread {
  static NSThread* thread = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    thread = [[NSThread alloc] initWithBlock:^{
      @autoreleasepool {
        NSRunLoop* loop = [NSRunLoop currentRunLoop];
        // A port with no traffic, so the run loop has a source and does not
        // return the moment it is entered.
        [loop addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode];
        while (true) {
          @autoreleasepool {
            [loop runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]];
          }
        }
      }
    }];
    thread.name = @"screenkit.net.upload";
    [thread start];
  });
  return thread;
}

- (instancetype)init {
  self = [super init];
  if (self == nil) return nil;
  _pending = [NSMutableArray array];
  _lock = [[NSRecursiveLock alloc] init];
  CFReadStreamRef read = nullptr;
  CFWriteStreamRef write = nullptr;
  CFStreamCreateBoundPair(kCFAllocatorDefault, &read, &write, 64 * 1024);
  _input = (__bridge_transfer NSInputStream*)read;
  _output = (__bridge_transfer NSOutputStream*)write;
  [self performSelector:@selector(openOnThread)
               onThread:[SKUploadPump thread]
             withObject:nil
          waitUntilDone:NO];
  return self;
}

- (void)openOnThread {
  [_lock lock];
  if (!_closed && !_opened) {
    _opened = YES;
    _output.delegate = self;
    [_output scheduleInRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    [_output open];
  }
  [_lock unlock];
}

- (BOOL)append:(NSData*)data {
  if (data.length == 0) return YES;
  BOOL accepted = YES;
  [_lock lock];
  if (_closed || _ended) {
    // Nothing to do, and nothing wrong: the body is already finished.
  } else if (_pendingBytes + data.length > kMaxPendingBytes) {
    accepted = NO;
  } else {
    [_pending addObject:data];
    _pendingBytes += data.length;
  }
  [_lock unlock];
  [self wake];
  return accepted;
}

- (void)finish {
  [_lock lock];
  _ended = YES;
  [_lock unlock];
  [self wake];
}

- (void)abort {
  [_lock lock];
  _closed = YES;
  [_pending removeAllObjects];
  _pendingBytes = 0;
  self.onDrained = nil;
  [_lock unlock];
  [self wake];
}

- (void)wake {
  [self performSelector:@selector(pump)
               onThread:[SKUploadPump thread]
             withObject:nil
          waitUntilDone:NO];
}

/// The stream thread, and only it.
- (void)pump {
  [_lock lock];
  if (_closed) {
    [self closeOutput];
    [_lock unlock];
    return;
  }
  if (!_opened) {
    [_lock unlock];
    return;
  }
  while (_pending.count > 0 && _output.hasSpaceAvailable) {
    NSData* head = _pending.firstObject;
    const NSInteger written = [_output write:static_cast<const uint8_t*>(head.bytes) + _offset
                                   maxLength:head.length - _offset];
    if (written <= 0) break;  // full, or the reader went away
    _offset += static_cast<NSUInteger>(written);
    _pendingBytes -= static_cast<NSUInteger>(written);
    if (_offset >= head.length) {
      _offset = 0;
      [_pending removeObjectAtIndex:0];
    }
  }
  // The reader's EOF is the write end closing, so it may not close a byte early.
  if (_ended && _pending.count == 0) {
    [self closeOutput];
    if (!_drained && !_closed) {
      _drained = YES;
      void (^done)(void) = self.onDrained;
      self.onDrained = nil;  // and with it whatever the block holds
      if (done != nil) done();
    }
  }
  [_lock unlock];
}

- (void)closeOutput {
  if (_output == nil) return;
  // Closing the write end is the reader's EOF, so on the abort path the read
  // end is shut first: an aborted upload must not hand the server a body that
  // ends cleanly, which is what a terminating chunk would say.
  if (_closed && _input != nil) {
    [_input close];
    _input = nil;
  }
  [_output close];
  _output.delegate = nil;
  _output = nil;
}

- (void)stream:(NSStream*)stream handleEvent:(NSStreamEvent)event {
  (void)stream;
  switch (event) {
    case NSStreamEventHasSpaceAvailable:
      [self pump];
      break;
    case NSStreamEventErrorOccurred:
    case NSStreamEventEndEncountered:
      [self abort];
      break;
    default:
      break;
  }
}

- (void)dealloc {
  // Normally nothing to do: the service always calls `abort`, which closes the
  // write end on the stream thread and keeps this object alive until it has.
  [_lock lock];
  _closed = YES;
  [_pending removeAllObjects];
  _pendingBytes = 0;
  self.onDrained = nil;
  if (_output != nil) {
    [_output close];
    _output.delegate = nil;
    _output = nil;
  }
  [_lock unlock];
}

@end

namespace screenkit::net {
namespace {

/// One in-flight request.
struct AppleRequest {
  std::uint64_t id = 0;
  std::shared_ptr<HttpSink> sink;
  RedirectMode redirect = RedirectMode::Follow;
  std::uint64_t flowWindow = 0;
  bool reportUpload = false;

  __strong NSURLSessionTask* task = nil;
  __strong SKUploadPump* pump = nil;

  bool headDelivered = false;
  bool redirected = false;
  bool finished = false;   // the sink has heard its last event
  bool cancelled = false;  // abortRequest or shutdown: the sink hears nothing more
  bool refusedRedirect = false;
  /// The trust challenge was answered with a refusal. NSURLSession reports that
  /// as a plain cancel, which is indistinguishable from this client's own, so
  /// the reason is kept here.
  bool refusedTrust = false;

  // Flow control: delivered to the sink and not yet acknowledged.
  std::uint64_t unacknowledged = 0;
  bool suspended = false;

  // Upload progress.
  std::uint64_t uploadSent = 0;
  bool uploadComplete = false;
  /// The bound pair is consumed once. A second ask is a retry this client
  /// cannot serve, because the chunks that were written into it are gone.
  bool pumpTaken = false;
};

/// One WebSocket.
struct AppleSocket {
  std::uint64_t id = 0;
  std::shared_ptr<SocketSink> sink;
  __strong NSURLSessionWebSocketTask* task = nil;
  bool opened = false;
  bool finished = false;
  bool cancelled = false;
  bool closeSent = false;
  bool refusedTrust = false;
};

}  // namespace
}  // namespace screenkit::net

// ---- the session delegate -------------------------------------------------------

@interface SKNetDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionWebSocketDelegate>
- (instancetype)initWithCore:(std::shared_ptr<screenkit::net::AppleCore>)core;
@end

@implementation SKNetDelegate {
  std::shared_ptr<screenkit::net::AppleCore> _core;
}

- (instancetype)initWithCore:(std::shared_ptr<screenkit::net::AppleCore>)core {
  self = [super init];
  if (self != nil) _core = std::move(core);
  return self;
}

- (std::shared_ptr<screenkit::net::AppleRequest>)requestForTask:(NSURLSessionTask*)task {
  const auto it = _core->requestsByTask.find(task.taskIdentifier);
  if (it == _core->requestsByTask.end()) return nullptr;
  if (it->second->cancelled || it->second->finished) return nullptr;
  return it->second;
}

- (std::shared_ptr<screenkit::net::AppleSocket>)socketForTask:(NSURLSessionTask*)task {
  const auto it = _core->socketsByTask.find(task.taskIdentifier);
  if (it == _core->socketsByTask.end()) return nullptr;
  if (it->second->cancelled) return nullptr;
  return it->second;
}

/// Forget a request: the sink has heard its last event, or never will.
- (void)retire:(const std::shared_ptr<screenkit::net::AppleRequest>&)request {
  request->finished = true;
  if (request->pump != nil) {
    [request->pump abort];
    request->pump = nil;
  }
  _core->requests.erase(request->id);
  if (request->task != nil) _core->requestsByTask.erase(request->task.taskIdentifier);
  request->sink.reset();
  request->task = nil;
}

// --- requests -------------------------------------------------------------------

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest*))completionHandler {
  (void)session;
  (void)response;
  auto live = [self requestForTask:task];
  if (live == nullptr) {
    completionHandler(nil);
    return;
  }
  switch (live->redirect) {
    case screenkit::net::RedirectMode::Follow:
      live->redirected = true;
      completionHandler(request);
      return;
    case screenkit::net::RedirectMode::Manual:
      // nil ends the task on the 3xx itself, which is what the shim turns into
      // an opaque redirect.
      completionHandler(nil);
      return;
    case screenkit::net::RedirectMode::Error:
      // The head must never reach the sink: mark it first, and
      // didCompleteWithError reports the redirect.
      live->refusedRedirect = true;
      completionHandler(nil);
      return;
  }
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  (void)session;
  auto live = [self requestForTask:task];
  if (live == nullptr) {
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  if (live->refusedRedirect) {
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  auto* http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)response : nil;
  if (http == nil) {
    auto sink = live->sink;
    screenkit::net::deliver(_core, [sink] {
      sink->onError(screenkit::net::NetError::Protocol, "the reply was not HTTP");
    });
    [self retire:live];
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  screenkit::net::ResponseHead head;
  head.status = static_cast<int>(http.statusCode);
  head.statusText = screenkit::net::canonicalStatusText(head.status);
  NSString* url = http.URL.absoluteString;
  head.url = url != nil ? std::string(url.UTF8String) : std::string();
  head.redirected = live->redirected;
  head.headers = screenkit::net::headersOf(http);
  live->headDelivered = true;
  auto sink = live->sink;
  screenkit::net::deliver(_core, [sink, head] { sink->onHead(head); });
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
  (void)session;
  auto live = [self requestForTask:task];
  if (live == nullptr || data.length == 0) return;
  // Counted and, if need be, suspended *before* anything else, and nothing
  // expensive happens here at all: see the file comment for why a delegate that
  // falls behind turns `suspend` into a no-op. The NSData is retained, not
  // copied; the copy happens on the delivery queue.
  const std::uint64_t size = data.length;
  if (live->flowWindow > 0) {
    live->unacknowledged += size;
    if (!live->suspended && live->unacknowledged >= live->flowWindow) {
      // Not reading is the whole mechanism: a suspended task stops taking bytes
      // off the connection, the receive window closes and the server stalls.
      live->suspended = true;
      [task suspend];
    }
  }
  auto sink = live->sink;
  screenkit::net::deliver(_core, [sink, data] { sink->onData(screenkit::net::bytesOf(data)); });
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
   didSendBodyData:(int64_t)sent
    totalBytesSent:(int64_t)totalSent
totalBytesExpectedToSend:(int64_t)total {
  (void)session;
  (void)sent;
  auto live = [self requestForTask:task];
  if (live == nullptr) return;
  live->uploadSent = static_cast<std::uint64_t>(totalSent);
  if (!live->reportUpload || live->uploadComplete) return;
  // A streamed body has no length, so `totalSent >= total` can never say it is
  // done; the pump saying it drained is what does (SKUploadPump.onDrained).
  const bool complete = total > 0 && totalSent >= total;
  live->uploadComplete = complete;
  auto sink = live->sink;
  const std::uint64_t uploaded = live->uploadSent;
  const std::int64_t expected = total == NSURLSessionTransferSizeUnknown ? -1 : total;
  screenkit::net::deliver(_core, [sink, uploaded, expected, complete] {
    sink->onUploadProgress(uploaded, expected, complete);
  });
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
 needNewBodyStream:(void (^)(NSInputStream*))completionHandler {
  (void)session;
  auto live = [self requestForTask:task];
  // nil fails the task rather than hanging it, which is what a retry this
  // client cannot serve has to do.
  if (live == nullptr || live->pump == nil || live->pumpTaken) {
    completionHandler(nil);
    return;
  }
  live->pumpTaken = true;
  completionHandler(live->pump.input);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  (void)session;
  if (auto socket = [self socketForTask:task]) {
    [self finishSocket:socket error:error];
    return;
  }
  auto live = [self requestForTask:task];
  if (live == nullptr) {
    _core->requestsByTask.erase(task.taskIdentifier);
    return;
  }
  auto sink = live->sink;
  const auto fail = [&](screenkit::net::NetError kind, std::string message) {
    screenkit::net::deliver(_core, [sink, kind, message] { sink->onError(kind, message); });
    [self retire:live];
  };
  if (live->refusedRedirect) {
    fail(screenkit::net::NetError::Redirect, "the response was a redirect and redirect: 'error' was asked for");
    return;
  }
  if (error != nil) {
    if (live->refusedTrust) {
      fail(screenkit::net::NetError::Tls, "the server's certificate was refused");
      return;
    }
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) {
      // This client's own cancels retire the request first, so a cancel that
      // still finds it live came from somewhere else -- and the sink is owed an
      // answer, or the page's promise never settles.
      fail(screenkit::net::NetError::Network, screenkit::net::describe(error));
      return;
    }
    fail(screenkit::net::classifyError(error, live->headDelivered), screenkit::net::describe(error));
    return;
  }
  if (!live->headDelivered) {
    fail(screenkit::net::NetError::Protocol, "the request ended with no response");
    return;
  }
  screenkit::net::deliver(_core, [sink] { sink->onEnd(); });
  [self retire:live];
}

// --- trust ------------------------------------------------------------------------

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
  (void)session;
  if (_core->testAnchors.empty() ||
      ![challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust]) {
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
    return;
  }
  SecTrustRef trust = challenge.protectionSpace.serverTrust;
  NSString* host = challenge.protectionSpace.host;
  if (!screenkit::net::evaluateWithAnchors(trust, _core->testAnchors, host)) {
    // Refusing cancels the task, and a cancel carries no reason of its own:
    // without this the failure would be indistinguishable from an abort and the
    // request would simply never settle.
    if (auto live = [self requestForTask:task]) live->refusedTrust = true;
    if (auto socket = [self socketForTask:task]) socket->refusedTrust = true;
    completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
    return;
  }
  completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:trust]);
}

// --- sockets ------------------------------------------------------------------------

- (void)finishSocket:(const std::shared_ptr<screenkit::net::AppleSocket>&)socket error:(NSError*)error {
  if (socket->finished) return;
  socket->finished = true;
  auto sink = socket->sink;
  _core->sockets.erase(socket->id);
  if (socket->task != nil) _core->socketsByTask.erase(socket->task.taskIdentifier);
  socket->sink.reset();
  socket->task = nil;
  if (sink == nullptr) return;
  const auto fail = [&](screenkit::net::NetError kind, std::string message) {
    screenkit::net::deliver(_core, [sink, kind, message] {
      sink->onError(kind, message);
      sink->onClose(1006, "", false);
    });
  };
  if (socket->refusedTrust) {
    fail(screenkit::net::NetError::Tls, "the server's certificate was refused");
    return;
  }
  if (error != nil && !([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)) {
    fail(screenkit::net::classifyError(error, socket->opened), screenkit::net::describe(error));
    return;
  }
  if (error != nil) {
    // Cancelled without a close frame from either side: the connection was
    // dropped, which is 1006 unless didCloseWithCode already reported it.
    fail(screenkit::net::NetError::Network, "the socket closed");
    return;
  }
  screenkit::net::deliver(_core, [sink] { sink->onClose(1006, "", false); });
}

- (void)URLSession:(NSURLSession*)session
     webSocketTask:(NSURLSessionWebSocketTask*)task
  didOpenWithProtocol:(NSString*)protocol {
  (void)session;
  auto socket = [self socketForTask:task];
  if (socket == nullptr || socket->finished) return;
  socket->opened = true;
  // NSURLSession offers `permessage-deflate` on every socket and has no
  // property for what the server accepted, but the 101 is on the task: its
  // `Sec-WebSocket-Extensions` is what `WebSocket.extensions` reports in a
  // browser. An extension it never offered fails the handshake before this
  // (NSURLErrorBadServerResponse), so whatever is here was negotiated.
  std::string extensions;
  if (auto* head = [task.response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)task.response : nil) {
    NSString* value = [head valueForHTTPHeaderField:@"Sec-WebSocket-Extensions"];
    if (value != nil) extensions = value.UTF8String;
  }
  const std::string chosen = protocol != nil ? std::string(protocol.UTF8String) : std::string();
  auto sink = socket->sink;
  screenkit::net::deliver(_core, [sink, chosen, extensions] { sink->onOpen(chosen, extensions); });
}

- (void)URLSession:(NSURLSession*)session
     webSocketTask:(NSURLSessionWebSocketTask*)task
  didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
            reason:(NSData*)reason {
  (void)session;
  auto socket = [self socketForTask:task];
  if (socket == nullptr || socket->finished) return;
  socket->finished = true;
  auto sink = socket->sink;
  _core->sockets.erase(socket->id);
  _core->socketsByTask.erase(task.taskIdentifier);
  socket->sink.reset();
  socket->task = nil;
  if (sink == nullptr) return;
  std::string text;
  if (reason.length > 0) {
    text.assign(static_cast<const char*>(reason.bytes), reason.length);
  }
  const int status = static_cast<int>(code);
  screenkit::net::deliver(_core, [sink, status, text] { sink->onClose(status, text, true); });
}

@end

// ---- the service --------------------------------------------------------------------

namespace screenkit::net {
namespace {

class AppleNetService final : public NetService {
 public:
  explicit AppleNetService(NetConfig config) {
    core_ = std::make_shared<AppleCore>();
    core_->queue = dispatch_queue_create((config.name + ".net").c_str(),
                                         dispatch_queue_attr_make_with_qos_class(
                                             DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    core_->delivery = dispatch_queue_create((config.name + ".net.delivery").c_str(),
                                            dispatch_queue_attr_make_with_qos_class(
                                                DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    dispatch_queue_set_specific(core_->delivery, kDeliveryQueueKey, (void*)kDeliveryQueueKey, nullptr);
    core_->testAnchors = std::move(config.testTlsAnchors);

    // Ephemeral: the cookies are an `NSHTTPCookieStorage` of this runtime's own
    // rather than the process-wide one, and there is no URL cache. A shared
    // store would leak one app's cookies into the next runtime in the same
    // process and write them into the user's cookie file, which is not what a
    // TV app's jar meant; what it costs is persistence across a restart, which
    // the cookie decision already gave up (spec-platform-http-clients.md).
    NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.URLCache = nil;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.HTTPShouldSetCookies = YES;
    configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyAlways;
    configuration.waitsForConnectivity = NO;
    // The web has no default read timeout -- `AbortSignal.timeout` and
    // `xhr.timeout` are its answer, and both work -- so neither does this. A
    // week is NSURLSession's own ceiling for a resource.
    configuration.timeoutIntervalForRequest = 7 * 24 * 60 * 60;
    configuration.timeoutIntervalForResource = 7 * 24 * 60 * 60;
    NSString* agent = toNSString(config.userAgent);
    if (agent != nil) configuration.HTTPAdditionalHeaders = @{@"User-Agent" : agent};
    core_->cookies = configuration.HTTPCookieStorage;

    NSOperationQueue* delegateQueue = [[NSOperationQueue alloc] init];
    delegateQueue.maxConcurrentOperationCount = 1;
    delegateQueue.underlyingQueue = core_->queue;
    delegate_ = [[SKNetDelegate alloc] initWithCore:core_];
    core_->session = [NSURLSession sessionWithConfiguration:configuration
                                                   delegate:delegate_
                                              delegateQueue:delegateQueue];
  }

  ~AppleNetService() override { shutdown(); }

  void startRequest(std::uint64_t id, HttpRequestSpec spec, std::shared_ptr<HttpSink> sink) override {
    if (stopped_) return;
    auto core = core_;
    auto shared = std::make_shared<HttpRequestSpec>(std::move(spec));
    dispatch_async(core->queue, ^{
      if (core->stopped) return;
      @autoreleasepool {
        startOnQueue(core, id, *shared, sink);
      }
    });
  }

  void appendRequestBody(std::uint64_t id, std::shared_ptr<const Bytes> chunk) override {
    if (stopped_ || !chunk || chunk->empty()) return;
    auto core = core_;
    NSData* data = [NSData dataWithBytes:chunk->data() length:chunk->size()];
    dispatch_async(core->queue, ^{
      if (core->stopped) return;
      const auto it = core->requests.find(id);
      if (it == core->requests.end() || it->second->pump == nil) return;
      if ([it->second->pump append:data]) return;
      // The queue is full: the server has stopped taking the body and the page
      // keeps writing. Dropping the chunk would corrupt the upload silently, so
      // the request fails instead -- and the memory stays bounded.
      auto request = it->second;
      auto sink = request->sink;
      cancelRequest(core, request);
      if (sink != nullptr) {
        deliver(core, [sink] {
          sink->onError(NetError::Network, "the request body outran the server by more than 8 MiB");
        });
      }
    });
  }

  void finishRequestBody(std::uint64_t id) override {
    if (stopped_) return;
    auto core = core_;
    dispatch_async(core->queue, ^{
      if (core->stopped) return;
      const auto it = core->requests.find(id);
      if (it == core->requests.end() || it->second->pump == nil) return;
      [it->second->pump finish];
    });
  }

  void abortRequest(std::uint64_t id) override {
    if (stopped_) return;
    auto core = core_;
    dispatch_async(core->queue, ^{
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      request->cancelled = true;
      cancelRequest(core, request);
    });
  }

  void acknowledgeResponseData(std::uint64_t id, std::uint64_t bytes) override {
    if (stopped_) return;
    auto core = core_;
    dispatch_async(core->queue, ^{
      const auto it = core->requests.find(id);
      if (it == core->requests.end()) return;
      auto request = it->second;
      request->unacknowledged = bytes >= request->unacknowledged ? 0 : request->unacknowledged - bytes;
      if (request->suspended && request->unacknowledged < request->flowWindow) {
        request->suspended = false;
        [request->task resume];
      }
    });
  }

  void openSocket(std::uint64_t id, Url url, std::vector<std::string> protocols,
                  std::shared_ptr<SocketSink> sink) override {
    if (stopped_) return;
    auto core = core_;
    auto shared = std::make_shared<std::pair<Url, std::vector<std::string>>>(std::move(url), std::move(protocols));
    dispatch_async(core->queue, ^{
      if (core->stopped) return;
      @autoreleasepool {
        openSocketOnQueue(core, id, shared->first, shared->second, sink);
      }
    });
  }

  void sendSocket(std::uint64_t id, bool text, std::shared_ptr<const Bytes> payload) override {
    if (stopped_ || !payload) return;
    auto core = core_;
    const std::uint64_t size = payload->size();
    NSURLSessionWebSocketMessage* message = nil;
    if (text) {
      NSString* string = [[NSString alloc] initWithBytes:payload->data()
                                                  length:payload->size()
                                                encoding:NSUTF8StringEncoding];
      if (string == nil) {
        // A text frame must carry valid UTF-8 (RFC 6455 5.6). Sending an empty
        // one instead and reporting it as sent would tell the page its data
        // went when it never did, so this fails the connection as the
        // WebSocket standard's "fail the WebSocket connection" does.
        dispatch_async(core->queue, ^{
          if (core->stopped) return;
          const auto it = core->sockets.find(id);
          if (it == core->sockets.end()) return;
          auto socket = it->second;
          if (socket->finished || socket->cancelled) return;
          socket->finished = true;
          auto sink = socket->sink;
          core->sockets.erase(socket->id);
          if (socket->task != nil) core->socketsByTask.erase(socket->task.taskIdentifier);
          socket->sink.reset();
          [socket->task cancel];
          socket->task = nil;
          if (sink != nullptr) {
            deliver(core, [sink] {
              sink->onError(NetError::Protocol, "a text frame must be valid UTF-8");
              sink->onClose(1006, "", false);
            });
          }
        });
        return;
      }
      message = [[NSURLSessionWebSocketMessage alloc] initWithString:string];
    } else {
      message = [[NSURLSessionWebSocketMessage alloc]
          initWithData:[NSData dataWithBytes:payload->data() length:payload->size()]];
    }
    dispatch_async(core->queue, ^{
      if (core->stopped) return;
      const auto it = core->sockets.find(id);
      if (it == core->sockets.end() || it->second->task == nil) return;
      auto socket = it->second;
      [socket->task sendMessage:message
              completionHandler:^(NSError*) {
                // Sent or failed, `bufferedAmount` has to stop counting these
                // bytes: a failure is reported by the task's own completion,
                // and a page waiting for bufferedAmount to reach 0 must not
                // wait for bytes that will never go.
                dispatch_async(core->queue, ^{
                  if (core->stopped || socket->finished || socket->cancelled) return;
                  auto sink = socket->sink;
                  if (sink != nullptr) deliver(core, [sink, size] { sink->onSent(size); });
                });
              }];
    });
  }

  void closeSocket(std::uint64_t id, int code, std::string reason) override {
    if (stopped_) return;
    auto core = core_;
    NSData* data = reason.empty() ? nil : [NSData dataWithBytes:reason.data() length:reason.size()];
    dispatch_async(core->queue, ^{
      const auto it = core->sockets.find(id);
      if (it == core->sockets.end()) return;
      auto socket = it->second;
      if (socket->closeSent) return;
      socket->closeSent = true;
      // NSURLSessionWebSocketTask has no way to send a close frame with no
      // status code, so a bare close() goes out as 1000 -- a recorded
      // divergence from the RFC 6455 client, which sent an empty one.
      const NSURLSessionWebSocketCloseCode wire =
          code == 0 ? NSURLSessionWebSocketCloseCodeNormalClosure
                    : static_cast<NSURLSessionWebSocketCloseCode>(code);
      [socket->task cancelWithCloseCode:wire reason:data];
    });
  }

  std::string cookiesFor(const Url& url) override {
    if (stopped_) return std::string();
    NSURL* target = makeUrl(url);
    if (target == nil) return std::string();
    NSArray<NSHTTPCookie*>* all = [core_->cookies cookiesForURL:target];
    std::string out;
    for (NSHTTPCookie* cookie in all) {
      if (cookie.isHTTPOnly) continue;  // the JS view never sees one
      if (cookie.isSecure && !url.secure()) continue;
      if (!out.empty()) out += "; ";
      out += std::string(cookie.name.UTF8String) + "=" + std::string(cookie.value.UTF8String);
    }
    return out;
  }

  bool setCookie(const Url& url, const std::string& setCookie) override {
    if (stopped_) return false;
    NSURL* target = makeUrl(url);
    if (target == nil) return false;
    NSString* value = toNSString(setCookie);
    if (value == nil) return false;  // @{key: nil} raises
    NSArray<NSHTTPCookie*>* parsed =
        [NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie" : value} forURL:target];
    if (parsed.count == 0) return false;
    bool stored = false;
    for (NSHTTPCookie* cookie in parsed) {
      // The JS API can neither set an HttpOnly cookie nor replace one.
      if (cookie.isHTTPOnly) continue;
      if (cookie.isSecure && !url.secure()) continue;
      bool shadowed = false;
      for (NSHTTPCookie* existing in [core_->cookies cookiesForURL:target]) {
        if (existing.isHTTPOnly && [existing.name isEqualToString:cookie.name]) {
          shadowed = true;
          break;
        }
      }
      if (shadowed) continue;
      [core_->cookies setCookie:cookie];
      stored = true;
    }
    return stored;
  }

  void shutdown() override {
    std::lock_guard<std::mutex> lock(shutdownMutex_);
    if (stopped_.exchange(true)) return;
    auto core = core_;
    // Synchronous, so no request outlives the runtime: every task is cancelled
    // and every sink dropped before this returns.
    dispatch_sync(core->queue, ^{
      core->stopped = true;
      auto requests = std::move(core->requests);
      auto sockets = std::move(core->sockets);
      core->requests.clear();
      core->sockets.clear();
      core->requestsByTask.clear();
      core->socketsByTask.clear();
      for (auto& [id, request] : requests) {
        (void)id;
        request->cancelled = true;
        request->finished = true;
        if (request->pump != nil) {
          [request->pump abort];
          request->pump = nil;
        }
        // The same belt and braces as cancelRequest, for the same reason: the
        // flow window suspends tasks, and a cancel is more certain to land on a
        // running one.
        if (request->suspended) {
          request->suspended = false;
          [request->task resume];
        }
        [request->task cancel];
        request->task = nil;
        request->sink.reset();
      }
      for (auto& [id, socket] : sockets) {
        (void)id;
        socket->cancelled = true;
        socket->finished = true;
        [socket->task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
        socket->task = nil;
        socket->sink.reset();
      }
    });
    // ...and the sink calls already handed to the delivery queue: silenced, so
    // they are dropped rather than run, then drained, so the one that may be
    // running right now has returned before this does. Not from the delivery
    // queue itself -- a sink dropped there can hold the last reference to this
    // service -- where waiting on it would be waiting on ourselves; the flag
    // alone covers what is behind us on that queue.
    core->silenced.store(true);
    if (dispatch_get_specific(kDeliveryQueueKey) != kDeliveryQueueKey) {
      dispatch_sync(core->delivery, ^{
      });
    }
    // Releases the delegate, and with it this service's last reference cycle.
    [core->session invalidateAndCancel];
    core->session = nil;
    delegate_ = nil;
  }

 private:
  static NSURL* makeUrl(const Url& url) {
    NSString* text = toNSString(url.href());
    if (text == nil) return nil;  // URLWithString:nil raises
    return [NSURL URLWithString:text];
  }

  static void startOnQueue(const std::shared_ptr<AppleCore>& core, std::uint64_t id,
                           const HttpRequestSpec& spec, const std::shared_ptr<HttpSink>& sink) {
    const auto fail = [&](NetError kind, std::string message) {
      deliver(core, [sink, kind, message] { sink->onError(kind, message); });
    };
    // An id already in use is a caller error. Taking it would strand the request
    // that holds it -- still running, and out of reach of any abort; refusing
    // settles the new one instead (as the Linux and Android clients do).
    if (core->requests.count(id) != 0) {
      fail(NetError::Url, "that request id is already in use");
      return;
    }
    NSURL* url = makeUrl(spec.url);
    if (url == nil) {
      fail(NetError::Url, "not a URL this client can reach: " + spec.url.href());
      return;
    }
    NSString* method = toNSString(spec.method);
    if (method == nil) {
      fail(NetError::Url, "the request method is not valid UTF-8");
      return;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = method;
    request.HTTPShouldHandleCookies = spec.useCookies ? YES : NO;
    // Content sniffing off, as a browser's fetch has it. CFNetwork otherwise
    // holds back a `text/plain` response -- its head and its first bytes -- until
    // 512 bytes have arrived or the body has ended, so a server streaming
    // `text/plain` a line at a time reaches the page in one lump. Measured with
    // three 8-byte chunks 300 ms apart: the head at 0.61 s without this, at
    // 0.004 s and each chunk as it was written with it. The key is CFNetwork's
    // own (`_kCFURLConnectionPropertyShouldSniff`, what WebKit sets for the same
    // reason) and is not documented, so it is set through the documented
    // per-request property API rather than any private call, and
    // `net-streaming-response` fails if a later OS stops honouring it.
    [NSURLProtocol setProperty:@NO forKey:@"_kCFURLConnectionPropertyShouldSniff" inRequest:request];
    bool hasContentType = false;
    for (const auto& [name, value] : spec.headers) {
      NSString* field = toNSString(name);
      NSString* text = toNSString(value);
      if (field == nil || text == nil) {
        fail(NetError::Url, "a request header is not valid UTF-8: " + name);
        return;
      }
      if (!hasContentType && lower(name) == "content-type") hasContentType = true;
      [request setValue:text forHTTPHeaderField:field];
    }
    if (!hasContentType && spec.method == "POST" &&
        (spec.streamingBody || (spec.body && !spec.body->empty()))) {
      // A body whose type the page did not give -- an ArrayBuffer, a typeless
      // Blob -- has no Content-Type in a browser, but CFNetwork writes
      // `application/x-www-form-urlencoded` into a **POST** that has a body and
      // no type of its own, which is a lie about the bytes. An empty value is
      // the one way to stop it: the header goes out empty, and every server that
      // tests for a type at all reads that as none. POST only, because that is
      // the only method CFNetwork does it to -- measured: a PUT with the same
      // body goes out with no Content-Type at all, as in a browser, and so does
      // every other method here now.
      [request setValue:@"" forHTTPHeaderField:@"Content-Type"];
    }

    auto live = std::make_shared<AppleRequest>();
    live->id = id;
    live->sink = sink;
    live->redirect = spec.redirect;
    live->flowWindow = spec.flowWindow;
    live->reportUpload = spec.reportUpload;

    NSURLSessionTask* task = nil;
    if (spec.streamingBody) {
      live->pump = [[SKUploadPump alloc] init];
      // A streamed body's length is unknown, so `didSendBodyData` can never say
      // it is complete; the pump handing over its last byte is what does. Weak,
      // and by id: a strong `core` here would be a cycle through its own
      // requests map, and the request may be gone by the time this runs.
      std::weak_ptr<AppleCore> weakCore = core;
      const std::uint64_t requestId = id;
      live->pump.onDrained = ^{
        auto held = weakCore.lock();
        if (!held) return;
        dispatch_async(held->queue, ^{
          if (held->stopped) return;
          const auto found = held->requests.find(requestId);
          if (found == held->requests.end()) return;
          auto& done = found->second;
          if (!done->reportUpload || done->uploadComplete || done->sink == nullptr) return;
          done->uploadComplete = true;
          auto sink = done->sink;
          const std::uint64_t sent = done->uploadSent;
          deliver(held, [sink, sent] { sink->onUploadProgress(sent, -1, true); });
        });
      };
      // Both: `uploadTaskWithStreamedRequest:` is documented to ask the delegate
      // for the stream, and setting it on the request is what actually carries
      // it in practice. Either way one consumer -- `needNewBodyStream` hands out
      // the same pair, and only once.
      request.HTTPBodyStream = live->pump.input;
      task = [core->session uploadTaskWithStreamedRequest:request];
    } else if (spec.body && !spec.body->empty()) {
      NSData* body = [NSData dataWithBytes:spec.body->data() length:spec.body->size()];
      task = [core->session uploadTaskWithRequest:request fromData:body];
    } else {
      task = [core->session dataTaskWithRequest:request];
    }
    live->task = task;
    core->requests[id] = live;
    core->requestsByTask[task.taskIdentifier] = live;
    [task resume];
  }

  /// Stop a request without telling its sink anything: an abort, or shutdown.
  static void cancelRequest(const std::shared_ptr<AppleCore>& core,
                            const std::shared_ptr<AppleRequest>& request) {
    request->finished = true;
    core->requests.erase(request->id);
    if (request->task != nil) core->requestsByTask.erase(request->task.taskIdentifier);
    // The task first, then the pump. Closing the write end of the bound pair is
    // an EOF to the reader, which would hand the server a *complete* body -- the
    // opposite of what an aborted upload means.
    // Resumed first, which is how Apple documents getting a suspended task to
    // act on a cancel. Measured on macOS 26 a suspended data task does cancel
    // without it, so this is belt and braces rather than the thing that makes
    // the connection close -- and it costs nothing to keep it that way.
    if (request->suspended) {
      request->suspended = false;
      [request->task resume];
    }
    [request->task cancel];
    request->task = nil;
    if (request->pump != nil) {
      [request->pump abort];
      request->pump = nil;
    }
    request->sink.reset();
  }

  static void openSocketOnQueue(const std::shared_ptr<AppleCore>& core, std::uint64_t id, const Url& url,
                                const std::vector<std::string>& protocols,
                                const std::shared_ptr<SocketSink>& sink) {
    const auto fail = [&](NetError kind, std::string message) {
      deliver(core, [sink, kind, message] {
        sink->onError(kind, message);
        sink->onClose(1006, "", false);
      });
    };
    if (core->sockets.count(id) != 0) {
      fail(NetError::Url, "that socket id is already in use");
      return;
    }
    NSURL* target = makeUrl(url);
    if (target == nil) {
      fail(NetError::Url, "not a URL this client can reach: " + url.href());
      return;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target];
    if (!protocols.empty()) {
      std::string joined;
      for (const auto& protocol : protocols) {
        if (!joined.empty()) joined += ", ";
        joined += protocol;
      }
      NSString* offered = toNSString(joined);
      if (offered == nil) {
        fail(NetError::Url, "a subprotocol is not valid UTF-8");
        return;
      }
      [request setValue:offered forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
    }
    auto socket = std::make_shared<AppleSocket>();
    socket->id = id;
    socket->sink = sink;
    NSURLSessionWebSocketTask* task = [core->session webSocketTaskWithRequest:request];
    socket->task = task;
    core->sockets[id] = socket;
    core->socketsByTask[task.taskIdentifier] = socket;
    [task resume];
    receiveNext(core, socket);
  }

  /// One message at a time, each completion re-posted onto the net queue so the
  /// sink is only ever touched there.
  ///
  /// Both arguments by value: a block captures a C++ reference *as a reference*,
  /// so a completion handed a caller's local shared_ptr by reference reads it
  /// after the caller has returned.
  static void receiveNext(std::shared_ptr<AppleCore> core, std::shared_ptr<AppleSocket> socket) {
    if (socket->finished || socket->cancelled || socket->task == nil) return;
    NSURLSessionWebSocketTask* task = socket->task;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
      dispatch_async(core->queue, ^{
        @autoreleasepool {
          if (core->stopped || socket->finished || socket->cancelled) return;
          if (error != nil || message == nil) {
            // The task's own didCompleteWithError reports why; stopping the
            // read loop is all that is needed here.
            return;
          }
          if (auto sink = socket->sink) {
            // By byte length: `strlen` on UTF8String would cut the frame at an
            // embedded NUL, which is a legal character in a text message and
            // which the Android client passes through.
            const bool text = message.type == NSURLSessionWebSocketMessageTypeString;
            NSData* data = text ? [message.string dataUsingEncoding:NSUTF8StringEncoding] : message.data;
            deliver(core, [sink, text, data] { sink->onMessage(text, bytesOf(data)); });
          }
          receiveNext(core, socket);
        }
      });
    }];
  }

  std::shared_ptr<AppleCore> core_;
  __strong SKNetDelegate* delegate_ = nil;
  std::atomic<bool> stopped_{false};
  std::mutex shutdownMutex_;
};

}  // namespace

std::shared_ptr<NetService> NetService::create(NetConfig config) {
  return std::make_shared<AppleNetService>(std::move(config));
}

bool networkAvailable() { return true; }

}  // namespace screenkit::net
