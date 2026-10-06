//! Hand-written Zig bindings for the only C ABI that React Native's prebuilt
//! `hermesvm` actually exports: the Static Hermes runtime-support surface
//! (`_sh_*`), declared upstream in `include/hermes/VM/static_h.h` and
//! `sh_legacy_value.h`.
//!
//! Nothing here is generated and no C or C++ is compiled. Zig emits references
//! to `_sh_foo`, Mach-O prepends the underscore, and the result (`__sh_foo`) is
//! exactly what the framework's export trie contains.
//!
//! Deliberately NOT bound:
//!   * anything taking an `SHUnit*` (`_sh_ljs_get_by_id_rjs` and friends) --
//!     those need metadata only the `shermes` AOT compiler emits;
//!   * `_sh_push_locals` / `_sh_enter` -- `static inline` in the header, and
//!     they poke at `SHRuntime`'s fields, whose layout depends on build-time
//!     macros from `libhermesvm-config.h`, which the prebuilt does not ship.
//!     See the GC caveat in README.md.

const std = @import("std");

/// Opaque: the layout is config-dependent, so we only ever pass the pointer.
pub const SHRuntime = opaque {};

/// `struct HermesValueBase { union { uint64_t raw; double f64; }; }`
///
/// Size 8, alignment 8. The union mixes an integer and a float member, so it is
/// not a homogeneous float aggregate and AAPCS64 returns it in x0 -- which is
/// what Zig does for a single-u64 extern struct.
pub const SHLegacyValue = extern struct {
    raw: u64,

    pub fn tag(v: SHLegacyValue) i64 {
        return @as(i64, @bitCast(v.raw)) >> num_data_bits;
    }
    pub fn etag(v: SHLegacyValue) i64 {
        return @as(i64, @bitCast(v.raw)) >> (num_data_bits - 1);
    }
    pub fn isDouble(v: SHLegacyValue) bool {
        return @as(u64, @bitCast(v.tag())) < @as(u64, @bitCast(@as(i64, tag_first)));
    }
    pub fn isUndefined(v: SHLegacyValue) bool {
        return v.etag() == etag_undefined;
    }
    pub fn isNull(v: SHLegacyValue) bool {
        return v.etag() == etag_null;
    }
    pub fn isBool(v: SHLegacyValue) bool {
        return v.etag() == etag_bool;
    }
    pub fn isString(v: SHLegacyValue) bool {
        return v.tag() == tag_str;
    }
    pub fn isObject(v: SHLegacyValue) bool {
        return v.tag() == tag_object;
    }
    pub fn getDouble(v: SHLegacyValue) f64 {
        std.debug.assert(v.isDouble());
        return @bitCast(v.raw);
    }
    pub fn getBool(v: SHLegacyValue) bool {
        std.debug.assert(v.isBool());
        return (v.raw >> bool_bit_idx) & 1 != 0;
    }

    /// Mirrors the header's `_sh_ljs_double`: a double is stored bit-for-bit.
    pub fn double(d: f64) SHLegacyValue {
        return .{ .raw = @bitCast(d) };
    }
    pub fn undef() SHLegacyValue {
        return encodeRawETag(0, etag_undefined);
    }
    pub fn nul() SHLegacyValue {
        return encodeRawETag(0, etag_null);
    }
    pub fn boolean(b: bool) SHLegacyValue {
        return encodeRawETag(@as(u64, @intFromBool(b)) << bool_bit_idx, etag_bool);
    }

    /// Best-effort name for the JS type, derived from the tag alone.
    pub fn typeName(v: SHLegacyValue) []const u8 {
        if (v.isDouble()) return "number";
        return switch (v.etag()) {
            etag_undefined => "undefined",
            etag_null => "null",
            etag_bool => "boolean",
            etag_symbol => "symbol",
            else => switch (v.tag()) {
                tag_str => "string",
                tag_bigint => "bigint",
                tag_object => "object",
                else => "empty/invalid",
            },
        };
    }
};

// --- encoding constants, transcribed from sh_legacy_value.h -----------------
// HERMESVALUE_VERSION 2. Tags sit in the high 16 bits of the 64-bit word and
// are defined as small negative numbers: HVTag_First = (int8_t)0xf9 = -7.

const num_tag_exp_bits = 16;
const num_data_bits = 64 - num_tag_exp_bits; // 48

pub const tag_first: i64 = -7;
pub const tag_empty_invalid: i64 = -7;
pub const tag_undefined_null: i64 = -6;
pub const tag_bool_symbol: i64 = -5;
pub const tag_str: i64 = -3; // == HVTag_FirstPointer
pub const tag_bigint: i64 = -2;
pub const tag_object: i64 = -1;

// An "extended tag" takes one extra bit: ETag == Tag * 2 (+1 for the odd half).
pub const etag_undefined: i64 = tag_undefined_null * 2; // -12
pub const etag_null: i64 = tag_undefined_null * 2 + 1; // -11
pub const etag_bool: i64 = tag_bool_symbol * 2; // -10
pub const etag_symbol: i64 = tag_bool_symbol * 2 + 1; // -9

/// The bool payload lives in the most significant bit after the ETag.
const bool_bit_idx = num_data_bits - 2; // 46

/// `val | ((uint64_t)etag << (kHV_NumDataBits - 1))`.
///
/// The C cast sign-extends the negative etag to 64 bits and the shift then
/// discards everything above bit 63; masking to the 17 bits that survive is the
/// same result, stated in a way Zig cannot be accused of wrapping.
fn encodeRawETag(val: u64, comptime et: i64) SHLegacyValue {
    const bits: u64 = @as(u64, @bitCast(et)) & ((1 << (num_tag_exp_bits + 1)) - 1);
    return .{ .raw = val | (bits << (num_data_bits - 1)) };
}

// --- the imported C ABI ----------------------------------------------------
// Every one of these is present in the export trie of all three slices
// (macosx, tvos-arm64, ios-arm64) of hermesvm 260318099.0.2.

/// Create a runtime. `argc == 0` skips command-line parsing entirely.
pub extern fn _sh_init(argc: c_int, argv: ?[*]const ?[*:0]const u8) ?*SHRuntime;
pub extern fn _sh_done(shr: *SHRuntime) void;

/// `len == -1` means "call strlen".
pub extern fn _sh_asciiz_to_string(shr: *SHRuntime, str: [*:0]const u8, len: isize) SHLegacyValue;

pub extern fn _sh_ljs_new_object(shr: *SHRuntime) SHLegacyValue;
pub extern fn _sh_ljs_new_array(shr: *SHRuntime, size: u32) SHLegacyValue;

/// The JS `+` operator, with full ECMAScript coercion semantics.
pub extern fn _sh_ljs_add_rjs(shr: *SHRuntime, a: *const SHLegacyValue, b: *const SHLegacyValue) SHLegacyValue;
pub extern fn _sh_ljs_mul_rjs(shr: *SHRuntime, a: *const SHLegacyValue, b: *const SHLegacyValue) SHLegacyValue;
pub extern fn _sh_ljs_typeof(shr: *SHRuntime, v: *SHLegacyValue) SHLegacyValue;
pub extern fn _sh_ljs_to_double_rjs(shr: *SHRuntime, n: *const SHLegacyValue) f64;
pub extern fn _sh_ljs_strict_equal(a: SHLegacyValue, b: SHLegacyValue) bool;

/// By-*value* property access needs no SHUnit and no inline cache, which is the
/// only reason object work is reachable from a plain C embedder.
pub extern fn _sh_ljs_put_by_val_strict_rjs(shr: *SHRuntime, target: *SHLegacyValue, key: *SHLegacyValue, value: *SHLegacyValue) void;
pub extern fn _sh_ljs_get_by_val_with_receiver_rjs(shr: *SHRuntime, source: *SHLegacyValue, key: *SHLegacyValue, receiver: *SHLegacyValue) SHLegacyValue;
pub extern fn _sh_ljs_is_in_rjs(shr: *SHRuntime, name: *SHLegacyValue, obj: *SHLegacyValue) SHLegacyValue;
