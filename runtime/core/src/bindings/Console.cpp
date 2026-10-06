// Copyright (c) ScreenKit contributors. MIT.
#include "Console.h"

#include <memory>
#include <string>
#include <utility>

#include <screenkit/Log.h>

namespace jsi = facebook::jsi;

namespace screenkit {
namespace {

/// Read a string property, if it is there and is a non-empty string.
bool stringProperty(jsi::Runtime& runtime, const jsi::Object& object, const char* name,
                    std::string& out) {
  try {
    jsi::Value value = object.getProperty(runtime, name);
    if (!value.isString()) return false;
    std::string text = value.getString(runtime).utf8(runtime);
    if (text.empty()) return false;
    out = std::move(text);
    return true;
  } catch (const jsi::JSIException&) {
    return false;
  }
}

/// `String(value)`, except that plain objects and arrays go through
/// JSON.stringify first -- otherwise every object logs as "[object Object]",
/// which makes console useless for the one thing it is here to do.
std::string format(jsi::Runtime& runtime, const jsi::Value& value) {
  if (value.isObject()) {
    jsi::Object object = value.getObject(runtime);

    // Errors first: JSON.stringify(new Error('x')) is "{}", because message and
    // stack are non-enumerable. console.error(err) is the most common
    // diagnostic call there is, so it must not lose exactly the two fields
    // anyone is looking for.
    std::string text;
    if (stringProperty(runtime, object, "stack", text)) return text;
    if (stringProperty(runtime, object, "message", text)) {
      std::string name;
      if (stringProperty(runtime, object, "name", name)) return name + ": " + text;
      return text;
    }

    if (!object.isFunction(runtime)) {
      try {
        jsi::Object json = runtime.global().getPropertyAsObject(runtime, "JSON");
        jsi::Function stringify = json.getPropertyAsFunction(runtime, "stringify");
        jsi::Value out = stringify.call(runtime, value);
        if (out.isString()) return out.getString(runtime).utf8(runtime);
      } catch (const jsi::JSIException&) {
        // Cyclic, a throwing toJSON, a Proxy that objects -- fall through to
        // toString rather than letting console.log throw at the call site.
      }
    }
  }
  try {
    return value.toString(runtime).utf8(runtime);
  } catch (const jsi::JSIException&) {
    return "<unprintable>";
  }
}

jsi::Value makeLogger(jsi::Runtime& runtime, const std::string& name, LogLevel level,
                      const std::shared_ptr<std::string>& tag) {
  return jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, name), 0,
      [level, tag](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                   size_t count) -> jsi::Value {
        std::string line;
        for (size_t i = 0; i < count; ++i) {
          if (i > 0) line += ' ';
          line += format(rt, args[i]);
        }
        log(level, *tag, line);
        return jsi::Value::undefined();
      });
}

}  // namespace

void installConsole(jsi::Runtime& runtime, std::string tag) {
  auto shared = std::make_shared<std::string>(std::move(tag));
  jsi::Object console(runtime);

  // This object replaces the global `console` wholesale, so any name it omits
  // is not merely unstyled -- it is undefined, and calling it throws TypeError
  // and takes the bundle down. The aliases cost nothing and are what real
  // bundles reach for.
  struct Entry {
    const char* name;
    LogLevel level;
  };
  static constexpr Entry kEntries[] = {
      {"log", LogLevel::Log},     {"info", LogLevel::Log},   {"debug", LogLevel::Log},
      {"trace", LogLevel::Log},   {"dir", LogLevel::Log},    {"warn", LogLevel::Warn},
      {"error", LogLevel::Error},
  };
  for (const Entry& entry : kEntries) {
    console.setProperty(runtime, entry.name,
                        makeLogger(runtime, entry.name, entry.level, shared));
  }

  runtime.global().setProperty(runtime, "console", console);
}

void installRejectionTracker(jsi::Runtime& runtime, std::string tag) {
  auto shared = std::make_shared<std::string>(std::move(tag));
  try {
    jsi::Value internal = runtime.global().getProperty(runtime, "HermesInternal");
    if (!internal.isObject()) return;
    jsi::Object hermes = internal.getObject(runtime);
    jsi::Value enable = hermes.getProperty(runtime, "enablePromiseRejectionTracker");
    if (!enable.isObject() || !enable.getObject(runtime).isFunction(runtime)) return;

    jsi::Object options(runtime);
    options.setProperty(runtime, "allRejections", true);
    options.setProperty(
        runtime, "onUnhandled",
        jsi::Function::createFromHostFunction(
            runtime, jsi::PropNameID::forAscii(runtime, "onUnhandled"), 2,
            [shared](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args,
                     size_t count) -> jsi::Value {
              // (id, reason)
              const std::string reason =
                  count > 1 ? format(rt, args[1]) : std::string("<no reason>");
              log(LogLevel::Error, *shared, "Unhandled promise rejection: " + reason);
              return jsi::Value::undefined();
            }));
    enable.getObject(runtime).getFunction(runtime).callWithThis(runtime, hermes, options);
  } catch (const jsi::JSIException& e) {
    log(LogLevel::Warn, *shared,
        std::string("could not enable promise rejection tracking: ") + e.what());
  }
}

}  // namespace screenkit
