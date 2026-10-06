// Copyright (c) ScreenKit contributors. MIT.
#include "Bytes.h"

#include <cctype>
#include <cstring>

#if defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#else
#include <random>
#endif

namespace screenkit::net {

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
  static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  if (i < size) {
    std::uint32_t n = data[i] << 16;
    if (i + 1 < size) n |= data[i + 1] << 8;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t size) {
  std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  const auto rol = [](std::uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); };

  Bytes message(data, data + size);
  const std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8;
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0);
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));

  for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t* p = &message[chunk + i * 4];
      w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<std::uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
    digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
    digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
  }
  return digest;
}

bool validUtf8(const std::uint8_t* data, std::size_t size) {
  std::size_t i = 0;
  while (i < size) {
    const std::uint8_t c = data[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    int extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1F;
      min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0F;
      min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07;
      min = 0x10000;
    } else {
      return false;
    }
    if (i + extra >= size + 0 && i + extra > size - 1) return false;
    for (int k = 1; k <= extra; ++k) {
      const std::uint8_t cc = data[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

void randomBytes(std::uint8_t* out, std::size_t size) {
#if defined(__APPLE__)
  arc4random_buf(out, size);
#else
  static thread_local std::random_device device;
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(device());
#endif
}

std::string lowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string trimWhitespace(const std::string& s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
  return s.substr(begin, end - begin);
}

bool headerHasToken(const std::string& value, const std::string& token) {
  std::size_t start = 0;
  while (start <= value.size()) {
    std::size_t end = value.find(',', start);
    if (end == std::string::npos) end = value.size();
    if (equalsIgnoreCase(trimWhitespace(value.substr(start, end - start)), token)) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace screenkit::net
