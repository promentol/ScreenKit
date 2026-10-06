// Copyright (c) ScreenKit contributors. MIT.
//
// System fonts on Apple platforms, through CoreText.
//
// A face is handed to FreeType as a font file rebuilt from the tables CoreText
// returns, not read from the path the font lives at: many system fonts share one
// .ttc collection file, some are not readable from an app's sandbox on tvOS, and
// the tables are the face CoreText actually matched, whatever file holds it.
//
// Built with ARC.
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include "SystemFonts.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace screenkit::text {
namespace {

NSString* toNSString(const std::string& value) {
  return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

std::string toStdString(NSString* value) {
  const char* utf8 = value.UTF8String;
  return utf8 ? std::string(utf8) : std::string();
}

/// CoreText's weight trait (-1 to 1, NSFontWeight's scale) as a CSS weight.
int cssWeight(double trait) {
  static constexpr std::array<std::pair<double, int>, 9> kScale = {{
      {-0.8, 100}, {-0.6, 200}, {-0.4, 300}, {0.0, 400}, {0.23, 500},
      {0.3, 600},  {0.4, 700},  {0.56, 800}, {0.62, 900},
  }};
  int best = 400;
  double distance = 2;
  for (const auto& [value, weight] : kScale) {
    if (std::fabs(trait - value) < distance) {
      distance = std::fabs(trait - value);
      best = weight;
    }
  }
  return best;
}

/// The family names CoreText knows, spelled the way it matches them. Collected
/// once: listing every installed font is the slow part of a lookup.
NSArray<NSString*>* installedFamilies() {
  static NSArray<NSString*>* families;
  static std::once_flag once;
  std::call_once(once, [] {
    NSMutableSet<NSString*>* names = [NSMutableSet set];
    CTFontCollectionRef collection = CTFontCollectionCreateFromAvailableFonts(nullptr);
    NSArray* descriptors = CFBridgingRelease(CTFontCollectionCreateMatchingFontDescriptors(collection));
    CFRelease(collection);
    for (id descriptor in descriptors) {
      NSString* family = CFBridgingRelease(
          CTFontDescriptorCopyAttribute((__bridge CTFontDescriptorRef)descriptor, kCTFontFamilyNameAttribute));
      if (family.length > 0) [names addObject:family];
    }
    families = names.allObjects;
  });
  return families;
}

constexpr std::uint32_t kTagCff = 0x43464620;   // 'CFF '
constexpr std::uint32_t kTagCff2 = 0x43464632;  // 'CFF2'
constexpr std::uint32_t kOpenTypeCff = 0x4F54544F;  // 'OTTO'
constexpr std::uint32_t kTrueType = 0x00010000;

NSArray* matchingDescriptors(NSString* family) {
  NSDictionary* attributes = @{(__bridge NSString*)kCTFontFamilyNameAttribute : family};
  CTFontDescriptorRef query = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attributes);
  NSSet* mandatory = [NSSet setWithObject:(__bridge NSString*)kCTFontFamilyNameAttribute];
  NSArray* found = CFBridgingRelease(
      CTFontDescriptorCreateMatchingFontDescriptors(query, (__bridge CFSetRef)mandatory));
  CFRelease(query);
  return found;
}

void putU16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
  out[at] = value >> 8;
  out[at + 1] = value & 0xFF;
}

void putU32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  out[at] = value >> 24;
  out[at + 1] = (value >> 16) & 0xFF;
  out[at + 2] = (value >> 8) & 0xFF;
  out[at + 3] = value & 0xFF;
}

}  // namespace

std::vector<SystemFace> systemFaces(const std::string& family) {
  std::vector<SystemFace> faces;
  @autoreleasepool {
    NSString* wanted = toNSString(family);
    if (wanted.length == 0) return faces;
    NSArray* descriptors = matchingDescriptors(wanted);
    if (descriptors.count == 0) {
      // CSS matches family names case-insensitively; CoreText does not.
      for (NSString* installed in installedFamilies()) {
        if ([installed caseInsensitiveCompare:wanted] == NSOrderedSame) {
          descriptors = matchingDescriptors(installed);
          break;
        }
      }
    }
    for (id item in descriptors) {
      auto descriptor = (__bridge CTFontDescriptorRef)item;
      NSString* name = CFBridgingRelease(CTFontDescriptorCopyAttribute(descriptor, kCTFontNameAttribute));
      NSDictionary* traits = CFBridgingRelease(CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute));
      if (name.length == 0) continue;
      SystemFace face;
      face.name = toStdString(name);
      NSNumber* weight = traits[(__bridge NSString*)kCTFontWeightTrait];
      NSNumber* symbolic = traits[(__bridge NSString*)kCTFontSymbolicTrait];
      face.weight = cssWeight(weight ? weight.doubleValue : 0);
      face.italic = symbolic && (symbolic.unsignedIntValue & kCTFontItalicTrait) != 0;
      faces.push_back(std::move(face));
    }
  }
  return faces;
}

std::vector<std::uint8_t> systemFontData(const std::string& postscriptName) {
  std::vector<std::uint8_t> file;
  @autoreleasepool {
    NSString* wanted = toNSString(postscriptName);
    if (wanted.length == 0) return file;
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)wanted, 16, nullptr);
    if (!font) return file;
    // CTFontCreateWithName substitutes a fallback for a name it does not know.
    NSString* actual = CFBridgingRelease(CTFontCopyPostScriptName(font));
    NSArray* tags = CFBridgingRelease(CTFontCopyAvailableTables(font, kCTFontTableOptionNoOptions));
    if (![actual isEqualToString:wanted] || tags.count == 0) {
      CFRelease(font);
      return file;
    }

    struct Table {
      std::uint32_t tag;
      NSData* data;
    };
    std::vector<Table> tables;
    bool cff = false;
    for (NSUInteger i = 0; i < tags.count; ++i) {
      // The array holds the tags themselves, not NSNumbers.
      const auto tag = static_cast<CTFontTableTag>(
          reinterpret_cast<std::uintptr_t>(CFArrayGetValueAtIndex((__bridge CFArrayRef)tags, i)));
      NSData* data = CFBridgingRelease(CTFontCopyTable(font, tag, kCTFontTableOptionNoOptions));
      if (!data) continue;
      if (tag == kTagCff || tag == kTagCff2) cff = true;
      tables.push_back({tag, data});
    }
    CFRelease(font);
    if (tables.empty()) return file;
    std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.tag < b.tag; });

    // An sfnt file: the offset table, one record per table, then the tables,
    // each padded to four bytes (OpenType spec, "Organization of an OpenType Font").
    const auto count = static_cast<std::uint16_t>(tables.size());
    std::uint16_t selector = 0;
    while ((2u << selector) <= count) ++selector;
    const std::uint16_t range = static_cast<std::uint16_t>((1u << selector) * 16);
    std::size_t size = 12 + 16 * tables.size();
    for (const auto& table : tables) size += (table.data.length + 3) & ~std::size_t{3};
    file.assign(size, 0);
    putU32(file, 0, cff ? kOpenTypeCff : kTrueType);
    putU16(file, 4, count);
    putU16(file, 6, range);
    putU16(file, 8, selector);
    putU16(file, 10, static_cast<std::uint16_t>(count * 16 - range));
    std::size_t offset = 12 + 16 * tables.size();
    std::size_t record = 12;
    for (const auto& table : tables) {
      const auto* bytes = static_cast<const std::uint8_t*>(table.data.bytes);
      const std::size_t length = table.data.length;
      std::copy(bytes, bytes + length, file.begin() + offset);
      std::uint32_t checksum = 0;
      for (std::size_t i = 0; i < length; i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4; ++j) word = (word << 8) | (i + j < length ? bytes[i + j] : 0);
        checksum += word;
      }
      putU32(file, record, table.tag);
      putU32(file, record + 4, checksum);
      putU32(file, record + 8, static_cast<std::uint32_t>(offset));
      putU32(file, record + 12, static_cast<std::uint32_t>(length));
      record += 16;
      offset += (length + 3) & ~std::size_t{3};
    }
  }
  return file;
}

std::string systemGenericFamily(const std::string& generic) {
  // What Safari resolves each generic to, then what tvOS carries in its place.
  static const std::array<std::pair<const char*, std::array<const char*, 3>>, 6> kGenerics = {{
      {"sans-serif", {"Helvetica", "Helvetica Neue", "Arial"}},
      {"serif", {"Times", "Times New Roman", "Georgia"}},
      {"monospace", {"Courier", "Courier New", "Menlo"}},
      {"cursive", {"Apple Chancery", "Snell Roundhand", "Zapfino"}},
      {"fantasy", {"Papyrus", "Chalkduster", "Marker Felt"}},
      {"system-ui", {"Helvetica Neue", "Helvetica", "Arial"}},
  }};
  for (const auto& [name, candidates] : kGenerics) {
    if (generic != name) continue;
    for (const char* candidate : candidates) {
      if (!systemFaces(candidate).empty()) return candidate;
    }
  }
  return "";
}

}  // namespace screenkit::text
