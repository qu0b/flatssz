const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const ssz_mod = b.addModule("ssz_runtime", .{
        .root_source_file = b.path("ssz_runtime.zig"),
    });

    const deneb_mod = b.addModule("deneb_ssz", .{
        .root_source_file = b.path("deneb_ssz.zig"),
        .imports = &.{
            .{ .name = "ssz_runtime", .module = ssz_mod },
        },
    });

    const exe = b.addExecutable(.{
        .name = "bench",
        .root_source_file = b.path("bench.zig"),
        .target = target,
        .optimize = optimize,
    });

    exe.root_module.addImport("ssz_runtime", ssz_mod);
    exe.root_module.addImport("deneb_ssz", deneb_mod);

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    const run_step = b.step("run", "Run the benchmark");
    run_step.dependOn(&run_cmd.step);
}
