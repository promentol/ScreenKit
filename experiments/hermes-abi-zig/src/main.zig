//! Minimal proof that a Zig program can drive the Hermes VM through a pure C
//! ABI -- no C++ compiled, no JSI, no shim object file. The only input is the
//! `hermesvm.framework` that React Native already ships and ScreenKit already
//! pins in tools/prebuilts/manifest.json.
//!
//! What it proves, in order: the runtime starts, JS values round-trip, the
//! operators are the real ECMAScript ones (`40 + "2" === "402"`, not Zig's
//! idea of `+`), objects and arrays take and return properties, and the
//! runtime tears down cleanly.
//!
//! Run: zig build run

const std = @import("std");
const h = @import("hermes_abi.zig");

var checks: usize = 0;
var failures: usize = 0;

fn report(ok: bool, comptime label: []const u8, comptime fmt: []const u8, args: anytype) void {
    checks += 1;
    if (!ok) failures += 1;
    std.debug.print("  {s} {s:<34} -> " ++ fmt ++ "\n", .{ if (ok) "ok  " else "FAIL", label } ++ args);
}

/// Compare a JS value against a Zig string literal using JS `===`, which is the
/// only way to inspect string contents here: the ABI exports no function that
/// reads a JS string's bytes back out. See README.md, "What is missing".
fn jsEqStr(shr: *h.SHRuntime, v: h.SHLegacyValue, lit: [:0]const u8) bool {
    const s = h._sh_asciiz_to_string(shr, lit.ptr, -1);
    return h._sh_ljs_strict_equal(v, s);
}

fn str(shr: *h.SHRuntime, lit: [:0]const u8) h.SHLegacyValue {
    return h._sh_asciiz_to_string(shr, lit.ptr, -1);
}

fn typeOf(shr: *h.SHRuntime, v: h.SHLegacyValue) h.SHLegacyValue {
    var tmp = v;
    return h._sh_ljs_typeof(shr, &tmp);
}

pub fn main() !void {
    std.debug.print(
        \\hermes-abi-zig -- Hermes driven from Zig over the _sh_* C ABI
        \\nothing C++ is compiled or linked by this executable
        \\
        \\
    , .{});

    // --- the runtime -------------------------------------------------------
    const shr = h._sh_init(0, null) orelse {
        std.debug.print("  FAIL _sh_init returned null\n", .{});
        return error.HermesInitFailed;
    };
    defer h._sh_done(shr);
    report(true, "_sh_init(0, null)", "SHRuntime@0x{x}", .{@intFromPtr(shr)});

    // --- numbers: does a value survive the ABI boundary at all? ------------
    {
        const a = h.SHLegacyValue.double(40);
        const b = h.SHLegacyValue.double(2);
        const sum = h._sh_ljs_add_rjs(shr, &a, &b);
        report(
            sum.isDouble() and sum.getDouble() == 42,
            "40 + 2",
            "{s} {d}",
            .{ sum.typeName(), sum.getDouble() },
        );
    }

    // --- strings -----------------------------------------------------------
    {
        const a = str(shr, "Hello, ");
        const b = str(shr, "Hermes");
        const joined = h._sh_ljs_add_rjs(shr, &a, &b);
        report(
            joined.isString() and jsEqStr(shr, joined, "Hello, Hermes"),
            "\"Hello, \" + \"Hermes\"",
            "{s} === \"Hello, Hermes\"",
            .{joined.typeName()},
        );
    }

    // --- the payoff: these are ECMAScript semantics, not Zig's -------------
    // 40 + "2" must produce the *string* "402". If this passes, the operator
    // being executed is genuinely Hermes'.
    {
        const n = h.SHLegacyValue.double(40);
        const s = str(shr, "2");
        const mixed = h._sh_ljs_add_rjs(shr, &n, &s);
        report(
            mixed.isString() and jsEqStr(shr, mixed, "402"),
            "40 + \"2\"",
            "{s} === \"402\"  (coercion, not addition)",
            .{mixed.typeName()},
        );

        // ...and back the other way, through ToNumber.
        const back = h._sh_ljs_to_double_rjs(shr, &mixed);
        report(back == 402, "Number(40 + \"2\")", "number {d}", .{back});

        // Whereas `*` coerces the other direction.
        const six = h.SHLegacyValue.double(6);
        const seven = str(shr, "7");
        const product = h._sh_ljs_mul_rjs(shr, &six, &seven);
        report(
            product.isDouble() and product.getDouble() == 42,
            "6 * \"7\"",
            "{s} {d}",
            .{ product.typeName(), product.getDouble() },
        );
    }

    // --- typeof, evaluated by the VM ---------------------------------------
    {
        const n = h.SHLegacyValue.double(1);
        const t = typeOf(shr, n);
        report(jsEqStr(shr, t, "number"), "typeof 1", "\"number\"", .{});

        const u = h.SHLegacyValue.undef();
        const tu = typeOf(shr, u);
        report(jsEqStr(shr, tu, "undefined"), "typeof undefined", "\"undefined\"", .{});
    }

    // --- objects: property set/get by value --------------------------------
    {
        var obj = h._sh_ljs_new_object(shr);
        var key = str(shr, "answer");
        var val = h.SHLegacyValue.double(42);

        h._sh_ljs_put_by_val_strict_rjs(shr, &obj, &key, &val);
        const got = h._sh_ljs_get_by_val_with_receiver_rjs(shr, &obj, &key, &obj);
        report(
            got.isDouble() and got.getDouble() == 42,
            "obj.answer = 42; obj.answer",
            "{s} {d}",
            .{ got.typeName(), got.getDouble() },
        );

        const present = h._sh_ljs_is_in_rjs(shr, &key, &obj);
        report(
            present.isBool() and present.getBool(),
            "\"answer\" in obj",
            "{}",
            .{present.getBool()},
        );

        var missing = str(shr, "nope");
        const absent = h._sh_ljs_is_in_rjs(shr, &missing, &obj);
        report(
            absent.isBool() and !absent.getBool(),
            "\"nope\" in obj",
            "{}",
            .{absent.getBool()},
        );
    }

    // --- arrays ------------------------------------------------------------
    {
        var arr = h._sh_ljs_new_array(shr, 3);
        var idx = h.SHLegacyValue.double(1);
        var val = str(shr, "mid");
        h._sh_ljs_put_by_val_strict_rjs(shr, &arr, &idx, &val);

        const got = h._sh_ljs_get_by_val_with_receiver_rjs(shr, &arr, &idx, &arr);
        report(
            got.isString() and jsEqStr(shr, got, "mid"),
            "a = new Array(3); a[1] = \"mid\"",
            "a[1] === \"mid\"",
            .{},
        );

        var len_key = str(shr, "length");
        const len = h._sh_ljs_get_by_val_with_receiver_rjs(shr, &arr, &len_key, &arr);
        report(
            len.isDouble() and len.getDouble() == 3,
            "a.length",
            "{s} {d}",
            .{ len.typeName(), len.getDouble() },
        );
    }

    std.debug.print("\n  {d}/{d} checks passed\n", .{ checks - failures, checks });
    if (failures != 0) return error.ChecksFailed;
}
