// Copyright (c) ScreenKit contributors. MIT.
//
// Hands JSI's conformance suite (testlib.cpp) a SpiderMonkey runtime per test, and
// adds the one thing the suite cannot know about: precompiled stencils.
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <jsi/test/testlib.h>

#include "spidermonkey/SpiderMonkeyRuntime.h"

namespace facebook {
namespace jsi {

std::vector<RuntimeFactory> runtimeGenerators() {
  return {[] { return std::shared_ptr<Runtime>(screenkit::spidermonkey::makeSpiderMonkeyRuntime()); }};
}

}  // namespace jsi
}  // namespace facebook

namespace {

namespace jsi = facebook::jsi;
namespace sm = screenkit::spidermonkey;

class Bytes final : public jsi::Buffer {
 public:
  explicit Bytes(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  size_t size() const override { return bytes_.size(); }
  const uint8_t* data() const override { return bytes_.data(); }

 private:
  std::vector<std::uint8_t> bytes_;
};

// A script with an inner function that is only compiled when first called (the
// lazy case), a closure, and a function whose source is asked for.
const char* kScript =
    "var counter = (function () { var n = 0; return function (by) { n += by; return n; }; })();\n"
    "function later(x) { return [x, x * 2].map(function (v) { return v + 1; }).join(','); }\n"
    "counter(40) + counter(2);\n";

void checkStencil(bool eager) {
  const auto stencil = sm::compileToStencil(kScript, "stencil-test.js", eager);
  ASSERT_TRUE(sm::looksLikeStencil(stencil.data(), stencil.size()));
  ASSERT_TRUE(sm::isLoadableStencil(stencil.data(), stencil.size()));
  auto rt = sm::makeSpiderMonkeyRuntime();
  const jsi::Value result = rt->evaluateJavaScript(std::make_shared<Bytes>(stencil), "stencil-test.js");
  EXPECT_EQ(result.getNumber(), 82);  // counter(40) + counter(2): 40 + 42
  const jsi::Function later = rt->global().getPropertyAsFunction(*rt, "later");
  EXPECT_EQ(later.call(*rt, 1).getString(*rt).utf8(*rt), "2,3");
  // The source travels with the stencil: toString and stack traces still work.
  const std::string text = jsi::Value(*rt, later).toString(*rt).utf8(*rt);
  EXPECT_NE(text.find("function later(x)"), std::string::npos) << text;
}

TEST(SpiderMonkeyStencil, LazyRoundTrip) { checkStencil(false); }
TEST(SpiderMonkeyStencil, EagerRoundTrip) { checkStencil(true); }

TEST(SpiderMonkeyStencil, RefusesAnotherBuild) {
  auto stencil = sm::compileToStencil("1", "x.js");
  // Corrupt one byte of the build id: magic (8) + length (4) + id.
  stencil[12] ^= 0x20;
  EXPECT_TRUE(sm::looksLikeStencil(stencil.data(), stencil.size()));
  EXPECT_FALSE(sm::isLoadableStencil(stencil.data(), stencil.size()));
  auto rt = sm::makeSpiderMonkeyRuntime();
  EXPECT_THROW(rt->evaluateJavaScript(std::make_shared<Bytes>(stencil), "x.js"), jsi::JSIException);
}

TEST(SpiderMonkeyStencil, SyntaxErrorIsReported) {
  try {
    sm::compileToStencil("function (", "broken.js");
    FAIL() << "compiled a syntax error";
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("SyntaxError"), std::string::npos) << what;
    EXPECT_NE(what.find("broken.js:1"), std::string::npos) << what;
  }
}

}  // namespace
