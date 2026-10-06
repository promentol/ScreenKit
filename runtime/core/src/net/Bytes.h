// Copyright (c) ScreenKit contributors. MIT.
//
// The few byte-level primitives the HTTP and WebSocket clients need, written
// here rather than taken from a platform crypto library so the same code runs
// on every target: SHA-1 for Sec-WebSocket-Accept (not a security primitive
// there -- RFC 6455 uses it as a checksum of the key), base64, UTF-8
// validation, and unpredictable bytes for keys and frame masks.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace screenkit::net {

using Bytes = std::vector<std::uint8_t>;

std::string base64Encode(const std::uint8_t* data, std::size_t size);
std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t size);

/// Strict UTF-8: no overlongs, no surrogates, nothing above U+10FFFF.
bool validUtf8(const std::uint8_t* data, std::size_t size);

/// From the OS's cryptographic source.
void randomBytes(std::uint8_t* out, std::size_t size);

std::string lowerAscii(std::string s);
bool equalsIgnoreCase(const std::string& a, const std::string& b);
std::string trimWhitespace(const std::string& s);

/// A comma-separated header value holds `token`, ignoring case and spaces:
/// `Connection: keep-alive, Upgrade` holds "upgrade".
bool headerHasToken(const std::string& value, const std::string& token);

}  // namespace screenkit::net
