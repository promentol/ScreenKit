//! The Hermes version pin lives in tools/prebuilts/manifest.json and nowhere
//! else -- same rule runtime/cmake/HermesPrebuilt.cmake follows, so this
//! experiment cannot drift from the runtime it is probing.
//!
//! Unlike the CMake path this does not fetch on a cold cache; if the framework
//! is missing it says which command to run and stops.

const std = @import("std");
const builtin = @import("builtin");

pub fn build(b: *std.Build) void {
    // Explicit target, not native -- and that is load-bearing on this machine.
    //
    // The Xcode 26 SDK's libSystem.B.tbd declares the umbrella library for
    // [x86_64-macos, x86_64-maccatalyst, arm64e-macos, arm64e-maccatalyst]
    // only; plain `arm64-macos` appears solely in the later re-export
    // documents. Zig 0.15.2 reads the first document, finds no matching
    // target, and every libc symbol comes out undefined -- even for a
    // hello-world built with `-lc`. Naming the target explicitly makes the
    // build non-native, so Zig links its own bundled libSystem.tbd instead.
    // Drop this once Zig handles the multi-document tbd.
    const target = b.standardTargetOptions(.{
        .default_target = .{ .cpu_arch = builtin.cpu.arch, .os_tag = .macos },
    });
    const optimize = b.standardOptimizeOption(.{});

    const hermes = locateHermes(b, target.result);

    const exe = b.addExecutable(.{
        .name = "hermes-abi-zig",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });

    // libc only. No linkLibCpp, no addCSourceFile -- that is the whole point:
    // if this links, the C ABI is genuinely reachable without a C++ toolchain.
    exe.linkLibC();
    exe.addFrameworkPath(.{ .cwd_relative = hermes.framework_dir });
    exe.linkFramework("hermesvm");
    // The framework's install name is @rpath/hermesvm.framework/Versions/1/
    // hermesvm, so the loader needs the directory that holds the .framework.
    exe.addRPath(.{ .cwd_relative = hermes.framework_dir });

    b.installArtifact(exe);

    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
    if (b.args) |args| run.addArgs(args);
    b.step("run", "Build and run the POC").dependOn(&run.step);
}

const Hermes = struct {
    version: []const u8,
    framework_dir: []const u8,
};

fn locateHermes(b: *std.Build, result: std.Target) Hermes {
    const manifest_path = b.pathFromRoot("../../tools/prebuilts/manifest.json");
    const bytes = std.fs.cwd().readFileAlloc(b.allocator, manifest_path, 4 << 20) catch |err|
        std.debug.panic("cannot read {s}: {s}", .{ manifest_path, @errorName(err) });

    const parsed = std.json.parseFromSlice(std.json.Value, b.allocator, bytes, .{}) catch |err|
        std.debug.panic("cannot parse {s}: {s}", .{ manifest_path, @errorName(err) });

    const hermes = parsed.value.object.get("hermes").?.object;
    const version = hermes.get("version").?.string;

    if (result.os.tag != .macos)
        std.debug.panic("this experiment only wires up macOS; got {s}", .{@tagName(result.os.tag)});
    const target_key = switch (result.cpu.arch) {
        .aarch64 => "apple-macos-arm64",
        .x86_64 => "apple-macos-x86_64",
        else => std.debug.panic("unsupported arch {s}", .{@tagName(result.cpu.arch)}),
    };
    const framework_rel = hermes.get("targets").?.object.get(target_key).?.object.get("framework").?.string;

    const prebuilts = std.process.getEnvVarOwned(b.allocator, "SCREENKIT_PREBUILTS") catch
        b.pathJoin(&.{ std.posix.getenv("HOME") orelse "/tmp", ".screenkit", "prebuilts" });

    const framework = b.pathJoin(&.{ prebuilts, "hermes", version, "apple", framework_rel });
    std.fs.cwd().access(b.pathJoin(&.{ framework, "hermesvm" }), .{}) catch
        std.debug.panic(
            "hermesvm.framework not found at {s}\n  Run: node tools/prebuilts/fetch.mjs --dep hermes {s}",
            .{ framework, target_key },
        );

    return .{ .version = version, .framework_dir = std.fs.path.dirname(framework).? };
}
