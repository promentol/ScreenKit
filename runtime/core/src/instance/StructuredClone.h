// Copyright (c) ScreenKit contributors. MIT.
//
// What `postMessage` carries between two instances.
//
// The two runtimes share no values at all -- separate Hermes heaps, separate
// threads -- so every message is copied natively into this tree on the sender's
// JS thread and built again on the receiver's. There is no transfer and no
// `MessagePort`: nothing is moved, only copied.
//
// **The subset is deliberate** (spec-m9-iframe-instances): primitives, plain
// objects and arrays, and `ArrayBuffer` by value. Anything else -- a function, a
// symbol, a `Map`, a `Date`, a host object, a typed array's view semantics --
// throws `DataCloneError` at the sender rather than arriving as something the
// receiver cannot tell apart from the real thing. Cycles are preserved, as the
// structured clone algorithm preserves them, through back-references.
//
// This is not JSON: a JSON bridge between the two runtimes is exactly what the
// spec forbids, and it would lose `undefined`, `-0`, cycles and bytes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <jsi/jsi.h>

namespace screenkit {

struct CloneValue {
  enum class Kind {
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
    ArrayBuffer,
    /// A back-reference to an object or array already seen in this message, by
    /// its index in the memo. What makes a cyclic message arrive as a cycle.
    Reference,
  };

  Kind kind = Kind::Undefined;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<CloneValue> items;                        // Array
  std::vector<std::pair<std::string, CloneValue>> entries;  // Object
  std::vector<std::uint8_t> bytes;                      // ArrayBuffer
  std::size_t reference = 0;                            // Reference
  /// This node's own index in the memo, for Array and Object.
  std::size_t memo = 0;
};

/// Copy `value` out of `rt`. Throws `jsi::JSError` carrying a `DataCloneError`
/// for anything the subset cannot hold, naming what it was. JS thread.
std::shared_ptr<const CloneValue> cloneOut(facebook::jsi::Runtime& rt, const facebook::jsi::Value& value);

/// Build `value` again in `rt`. JS thread -- the receiver's, which is a
/// different one.
facebook::jsi::Value cloneIn(facebook::jsi::Runtime& rt, const CloneValue& value);

}  // namespace screenkit
