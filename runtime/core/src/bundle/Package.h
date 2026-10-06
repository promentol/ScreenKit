// Copyright (c) ScreenKit contributors. MIT.
//
// The `.skpkg` gate (Architecture.md 6.1).
//
// A package is a directory: manifest.json, the entry bytecode it names, and the
// app's assets. The manifest is read and gated before anything from the package
// is evaluated -- a bytecode version this engine does not speak, or a package
// built for a newer runtime, is a logged error and a failed launch rather than a
// fault somewhere inside the VM. The package directory then becomes the asset
// root, so HostIO's confinement applies to it unchanged.
//
// This lives in core rather than in the host shell because there are two callers
// now: `screenkit-host`, which gates the app it was asked to run, and an
// `<iframe>` instance, which gates the package a launcher's `src` names
// (core/src/instance/). One gate, one set of messages, one set of host rows
// asserting them.
#pragma once

#include <memory>
#include <string>

namespace screenkit {

class Runtime;

namespace bundle {

/// What to run and where its assets live, whichever form the path took.
struct LaunchTarget {
  std::string bundle;     // the file evaluateBundle runs
  std::string assetRoot;  // the directory asset reads are confined to
  bool package = false;
};

/// A directory is a package and is opened and gated; anything else is a plain
/// bundle file, run from the directory it sits in. Every refusal names the file
/// it is about and, for a version, both sides.
///
/// `tag` is the log tag the one success line is written under -- the host's, or
/// the instance's name.
///
/// Blocks on `runtime`'s JS thread to parse the manifest with its own
/// `JSON.parse`: no third-party JSON parser in the host, and nothing from the
/// package is evaluated -- the text is handed to `JSON.parse` as a string, never
/// as source. Must not be called from that JS thread.
bool resolveLaunch(const std::shared_ptr<Runtime>& runtime, const std::string& path,
                   const std::string& tag, LaunchTarget& target, std::string& error);

}  // namespace bundle
}  // namespace screenkit
