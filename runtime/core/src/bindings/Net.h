// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

class EventLoop;
class NetBinding;

/// Install `__screenkit.net`: the narrow handle API the DOM shim builds fetch,
/// XMLHttpRequest, WebSocket and EventSource on (runtime/js/README.md).
///
///   request({method, url, headers: [[name, value]...], body?, stream?, redirect?,
///            cookies?, upload?}, onEvent) -> id
///       onEvent('head', {status, statusText, url, redirected, headers})
///       onEvent('data', ArrayBuffer) ... onEvent('end')
///       onEvent('upload', {loaded, total, complete})
///       onEvent('error', {kind, message})
///   write(id, bytes) · finish(id) · abort(id)
///   openSocket(url, protocols, onEvent) -> id
///       onEvent('open', {protocol, extensions}) · ('message', string|ArrayBuffer)
///       onEvent('sent', bytes) · ('error', {kind, message}) · ('close', {code, reason, wasClean})
///   send(id, string|bytes) · close(id, code, reason)
///   getCookies(url) -> string · setCookie(url, cookie) -> bool
///
/// Every call returns at once; events arrive later as event-loop tasks through
/// `executor` -- so they wait behind a paused runtime's freeze gate like any
/// other task -- in order, with a microtask checkpoint after each. A request or
/// socket keeps at most one task queued at a time (Net.cpp, EventChannel). From
/// `request`/`openSocket` until the terminal event is delivered (or `abort`), the
/// runtime's loop is held non-idle.
///
/// `__screenkit` must already exist (installHostIO). JS thread only.
std::shared_ptr<NetBinding> installNet(facebook::jsi::Runtime& runtime,
                                       std::shared_ptr<JsExecutor> executor,
                                       std::shared_ptr<EventLoop> loop, const RuntimeConfig& config);

/// Close every request and socket, wait for the I/O queue to let go of them,
/// and drop every JS callback -- while the runtime still exists. JS thread only;
/// idempotent.
void shutdownNet(NetBinding& binding);

}  // namespace screenkit
