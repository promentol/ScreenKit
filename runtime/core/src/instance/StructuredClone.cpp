// Copyright (c) ScreenKit contributors. MIT.
#include "StructuredClone.h"

#include <cmath>

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// Objects and arrays seen so far in this message, so a repeat is a
/// back-reference rather than a second copy, and a cycle terminates.
struct OutMemo {
  std::vector<jsi::Object> seen;
  std::vector<std::size_t> index;
};

[[noreturn]] void dataCloneError(jsi::Runtime& rt, const std::string& what) {
  // The shape a DOMException has in the shim: a name the page can switch on,
  // and a message that says what it actually tripped over.
  jsi::Object error(rt);
  error.setProperty(rt, "name", jsi::String::createFromAscii(rt, "DataCloneError"));
  error.setProperty(rt, "message",
                    jsi::String::createFromUtf8(rt, what + " could not be cloned: postMessage "
                                                           "carries primitives, plain objects and "
                                                           "arrays, and ArrayBuffer"));
  throw jsi::JSError(rt, jsi::Value(rt, error));
}

std::string describe(jsi::Runtime& rt, const jsi::Value& value) {
  if (value.isSymbol()) return "a symbol";
  if (!value.isObject()) return "a value of this type";
  jsi::Object object = value.getObject(rt);
  if (object.isFunction(rt)) return "a function";
  if (object.isHostObject(rt)) return "a host object";
  jsi::Value constructor = object.getProperty(rt, "constructor");
  if (constructor.isObject()) {
    jsi::Value name = constructor.getObject(rt).getProperty(rt, "name");
    if (name.isString()) {
      const std::string text = name.getString(rt).utf8(rt);
      if (!text.empty() && text != "Object") return "a " + text;
    }
  }
  return "that object";
}

/// A plain object is one whose prototype is `Object.prototype` or null: anything
/// with a class of its own would arrive here as a bag of properties with the
/// class silently gone, which is worse than refusing it.
bool isPlainObject(jsi::Runtime& rt, const jsi::Object& object) {
  jsi::Function getPrototypeOf = rt.global()
                                     .getPropertyAsObject(rt, "Object")
                                     .getPropertyAsFunction(rt, "getPrototypeOf");
  jsi::Value prototype = getPrototypeOf.call(rt, jsi::Value(rt, object));
  if (prototype.isNull()) return true;
  if (!prototype.isObject()) return false;
  jsi::Value objectPrototype =
      rt.global().getPropertyAsObject(rt, "Object").getProperty(rt, "prototype");
  if (!objectPrototype.isObject()) return false;
  return jsi::Object::strictEquals(rt, objectPrototype.getObject(rt), prototype.getObject(rt));
}

/// How deep a message may nest. Cycles are memoised, so this is only ever
/// reached by a genuinely deep structure -- a long linked list, say -- and the
/// encoder recurses, so without a cap that is a stack overflow rather than an
/// error the page can catch. The shim's own `structuredClone` caps at the same
/// depth, so both refuse the same values.
///
/// The number has to be reached before *any* engine's own stack guard is, and
/// this frame is not small: at 128 an ASan build spent enough native stack per
/// level that Hermes raised `RangeError` first, and the page saw an engine
/// overflow where the matrix promises `DataCloneError` (`iframe-message` under
/// macos-asan). Sixty-four leaves that margin and is still far deeper than any
/// real message.
constexpr int kMaxCloneDepth = 64;

CloneValue encode(jsi::Runtime& rt, const jsi::Value& value, OutMemo& memo, std::size_t& next,
                  int depth) {
  CloneValue out;
  if (depth > kMaxCloneDepth) {
    dataCloneError(rt, "a value nested more than " + std::to_string(kMaxCloneDepth) + " deep");
  }
  if (value.isUndefined()) {
    out.kind = CloneValue::Kind::Undefined;
    return out;
  }
  if (value.isNull()) {
    out.kind = CloneValue::Kind::Null;
    return out;
  }
  if (value.isBool()) {
    out.kind = CloneValue::Kind::Boolean;
    out.boolean = value.getBool();
    return out;
  }
  if (value.isNumber()) {
    out.kind = CloneValue::Kind::Number;
    out.number = value.getNumber();
    return out;
  }
  if (value.isString()) {
    out.kind = CloneValue::Kind::String;
    out.string = value.getString(rt).utf8(rt);
    return out;
  }
  if (!value.isObject()) dataCloneError(rt, describe(rt, value));

  jsi::Object object = value.getObject(rt);
  if (object.isFunction(rt) || object.isHostObject(rt)) dataCloneError(rt, describe(rt, value));

  if (object.isArrayBuffer(rt)) {
    jsi::ArrayBuffer buffer = object.getArrayBuffer(rt);
    out.kind = CloneValue::Kind::ArrayBuffer;
    const std::uint8_t* data = buffer.data(rt);
    out.bytes.assign(data, data + buffer.size(rt));
    return out;
  }

  for (std::size_t i = 0; i < memo.seen.size(); ++i) {
    if (!jsi::Object::strictEquals(rt, memo.seen[i], object)) continue;
    out.kind = CloneValue::Kind::Reference;
    out.reference = memo.index[i];
    return out;
  }

  if (object.isArray(rt)) {
    out.kind = CloneValue::Kind::Array;
    out.memo = next++;
    memo.seen.emplace_back(jsi::Value(rt, object).getObject(rt));
    memo.index.push_back(out.memo);
    jsi::Array array = object.getArray(rt);
    const std::size_t length = array.size(rt);
    out.items.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      out.items.push_back(encode(rt, array.getValueAtIndex(rt, i), memo, next, depth + 1));
    }
    return out;
  }

  if (!isPlainObject(rt, object)) dataCloneError(rt, describe(rt, value));

  out.kind = CloneValue::Kind::Object;
  out.memo = next++;
  memo.seen.emplace_back(jsi::Value(rt, object).getObject(rt));
  memo.index.push_back(out.memo);
  jsi::Array names = object.getPropertyNames(rt);
  const std::size_t count = names.size(rt);
  out.entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    jsi::Value name = names.getValueAtIndex(rt, i);
    if (!name.isString()) continue;  // a symbol key is not part of the clone
    const std::string key = name.getString(rt).utf8(rt);
    out.entries.emplace_back(key,
                             encode(rt, object.getProperty(rt, key.c_str()), memo, next, depth + 1));
  }
  return out;
}

class CloneBuffer final : public jsi::MutableBuffer {
 public:
  explicit CloneBuffer(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  uint8_t* data() override { return bytes_.data(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

jsi::Value decode(jsi::Runtime& rt, const CloneValue& value, std::vector<jsi::Value>& memo) {
  switch (value.kind) {
    case CloneValue::Kind::Undefined:
      return jsi::Value::undefined();
    case CloneValue::Kind::Null:
      return jsi::Value::null();
    case CloneValue::Kind::Boolean:
      return jsi::Value(value.boolean);
    case CloneValue::Kind::Number:
      return jsi::Value(value.number);
    case CloneValue::Kind::String:
      return jsi::String::createFromUtf8(rt, value.string);
    case CloneValue::Kind::ArrayBuffer:
      return jsi::ArrayBuffer(rt, std::make_shared<CloneBuffer>(value.bytes));
    case CloneValue::Kind::Reference:
      return value.reference < memo.size() ? jsi::Value(rt, memo[value.reference])
                                           : jsi::Value::undefined();
    case CloneValue::Kind::Array: {
      jsi::Array array(rt, value.items.size());
      // Recorded before the elements are built, so an element that refers back
      // to this array finds it. `jsi::Value` is move-only, so the memo is grown
      // one element at a time rather than resized with a fill value.
      while (memo.size() <= value.memo) memo.emplace_back(jsi::Value::undefined());
      memo[value.memo] = jsi::Value(rt, array);
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        array.setValueAtIndex(rt, i, decode(rt, value.items[i], memo));
      }
      return jsi::Value(rt, array);
    }
    case CloneValue::Kind::Object: {
      jsi::Object object(rt);
      while (memo.size() <= value.memo) memo.emplace_back(jsi::Value::undefined());
      memo[value.memo] = jsi::Value(rt, object);
      for (const auto& entry : value.entries) {
        object.setProperty(rt, entry.first.c_str(), decode(rt, entry.second, memo));
      }
      return jsi::Value(rt, object);
    }
  }
  return jsi::Value::undefined();
}

}  // namespace

std::shared_ptr<const CloneValue> cloneOut(jsi::Runtime& rt, const jsi::Value& value) {
  OutMemo memo;
  std::size_t next = 0;
  auto out = std::make_shared<CloneValue>(encode(rt, value, memo, next, 0));
  return out;
}

jsi::Value cloneIn(jsi::Runtime& rt, const CloneValue& value) {
  std::vector<jsi::Value> memo;
  return decode(rt, value, memo);
}

}  // namespace screenkit
