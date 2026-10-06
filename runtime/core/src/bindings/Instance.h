// Copyright (c) ScreenKit contributors. MIT.
#pragma once

#include <memory>

#include <jsi/jsi.h>

#include <screenkit/Runtime.h>

namespace screenkit {

class EventLoop;
class InstanceBinding;
class RuntimeControl;

/// Install `__screenkit.instances`: the narrow handle API `HTMLIFrameElement`
/// is built on (runtime/js/README.md, "Instances"), over `screenkit::Instance`.
///
///   capabilities() -> {canEmbed, hasParent, sandboxed, allowNetwork,
///                      allowMedia, allowStorage, allowBackgroundAudio,
///                      allowBackgroundTimers}
///   onevent = function (target, type, payload) {...}      set by the shim
///   onparentmessage = function (data) {...}               set by the shim
///
///   in a page that may embed (capabilities().canEmbed):
///     create(target) -> id            one instance; events go to `target`
///     load(id, {src, sandbox?, width?, height?}) -> serial
///     focus(id)                       id 0 focuses this page itself
///     setPaused(id, boolean)
///     post(id, message)               parent -> child, structured-clone subset
///     setPlane(id, x, y, width, height, visible, order, opacity)
///     unload(id)                      the element left the document
///     destroy(id)
///
///   in a page that is embedded (capabilities().hasParent):
///     parentPost(message)             child -> parent
///     parentFocus()                   `window.parent.focus()`
///
///   events, each payload carrying the `serial` of the load it belongs to:
///     load   {}                       the package ran its entry
///     error  {message}                the gate refused it, or the entry threw
///     message {data}                  child -> parent postMessage
///     focus  {} / blur {}             the instance took or lost the remote
///
/// `src` names a **local package only**: a path confined to the parent's own
/// package, which the asset root already enforces. A launcher ships the games it
/// embeds, so nothing is downloaded -- an `http(s)` src is refused with a
/// documented error and fetching one stays M12's job.
///
/// `sandbox` is the element's token list. An element with no `sandbox` attribute
/// is not sandboxed; one with the attribute gets back only what its tokens name,
/// and the gate is applied where each binding is installed, so nothing reachable
/// from the instance's JS can widen it (screenkit/Instance.h).
///
/// **This is not an isolation boundary** (Architecture.md 5.2): JS is isolated,
/// memory is not, and a native crash or an OOM in any instance takes every
/// instance with it including the launcher.
///
/// Events arrive as event-loop tasks through `executor`, in order per instance,
/// behind the freeze gate -- like the network's and the player's. An instance is
/// released by `destroy`, when `target` is collected, or at shutdown; until then
/// the runtime's loop is held non-idle.
///
/// `__screenkit` must already exist (installHostIO). JS thread only.
/// `control` is how `focus()` freezes and thaws the runtime this binding runs on
/// -- a launcher pauses itself when the game it embeds takes the remote. The
/// `Runtime` wrapper does not exist yet when bindings are installed, which is
/// why it is not one (instance/InstanceImpl.h).
std::shared_ptr<InstanceBinding> installInstances(facebook::jsi::Runtime& runtime,
                                                  std::shared_ptr<JsExecutor> executor,
                                                  std::shared_ptr<EventLoop> loop,
                                                  std::shared_ptr<RuntimeControl> control,
                                                  const RuntimeConfig& config);

/// Terminate every instance -- synchronously, so each thread is joined and
/// nothing is delivered afterwards -- and drop every JS reference while the
/// runtime still exists. JS thread only; idempotent.
void shutdownInstances(InstanceBinding& binding);

}  // namespace screenkit
