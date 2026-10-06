# JSI, vendored

JSI's headers, its implementation (`jsi/jsi.cpp`) and its conformance suite
(`jsi/test/`), byte for byte from facebook/hermes at `4188b63f24026ca62b79dc81d95db122a59c6089` -- the
commit `tools/prebuilts/manifest.json` pins for the Hermes prebuilts. MIT,
`LICENSE`.

Written by `tools/vendor/jsi.sh`; do not edit. A Hermes bump re-runs it.
The Hermes build takes JSI from the prebuilt and never compiles these; they
exist for runtimes on other engines (`runtime/core/src/spidermonkey/`).
