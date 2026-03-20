const std = @import("std");
const ssz = @import("ssz_runtime.zig");
const deneb = @import("deneb_ssz.zig");

const SignedBeaconBlock = deneb.SignedBeaconBlock;

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // Read test data
    const data = try std.fs.cwd().readFileAlloc(allocator, "block-mainnet.ssz", 1024 * 1024);
    defer allocator.free(data);

    // Read expected HTR from meta JSON
    const meta_json = try std.fs.cwd().readFileAlloc(allocator, "block-mainnet-meta.json", 4096);
    defer allocator.free(meta_json);

    const expected_htr_hex = blk: {
        // Simple JSON parse: find "htr": "..." value
        const htr_key = "\"htr\": \"";
        const start = std.mem.indexOf(u8, meta_json, htr_key) orelse return error.MetaParseFailed;
        const val_start = start + htr_key.len;
        const val_end = std.mem.indexOfPos(u8, meta_json, val_start, "\"") orelse return error.MetaParseFailed;
        break :blk meta_json[val_start..val_end];
    };

    var expected_htr: [32]u8 = undefined;
    for (0..32) |i| {
        expected_htr[i] = std.fmt.parseInt(u8, expected_htr_hex[i * 2 .. i * 2 + 2], 16) catch return error.HexParseFailed;
    }

    std.debug.print("SSZ data size: {} bytes\n", .{data.len});

    const warmup_iters: usize = 100;
    const bench_iters: usize = 1000;

    // --- Unmarshal benchmark ---
    {
        // Warmup
        var i: usize = 0;
        while (i < warmup_iters) : (i += 1) {
            _ = try SignedBeaconBlock.fromSszBytes(data);
        }

        var timer = try std.time.Timer.start();
        i = 0;
        while (i < bench_iters) : (i += 1) {
            _ = try SignedBeaconBlock.fromSszBytes(data);
        }
        const elapsed_ns = timer.read();
        const us_per_op = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(bench_iters)) / 1000.0;
        std.debug.print("unmarshal: {d:.1} us/op\n", .{us_per_op});
    }

    // --- Marshal benchmark ---
    {
        const block = try SignedBeaconBlock.fromSszBytes(data);

        // Warmup
        var i: usize = 0;
        while (i < warmup_iters) : (i += 1) {
            const buf = try block.toSszBytes(allocator);
            allocator.free(buf);
        }

        var timer = try std.time.Timer.start();
        i = 0;
        while (i < bench_iters) : (i += 1) {
            const buf = try block.toSszBytes(allocator);
            allocator.free(buf);
        }
        const elapsed_ns = timer.read();
        const us_per_op = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(bench_iters)) / 1000.0;
        std.debug.print("marshal: {d:.1} us/op\n", .{us_per_op});
    }

    // --- Hash Tree Root benchmark ---
    {
        const block = try SignedBeaconBlock.fromSszBytes(data);

        // Warmup
        var i: usize = 0;
        while (i < warmup_iters) : (i += 1) {
            var h = ssz.Hasher.init(allocator);
            defer h.deinit();
            block.treeHashWith(&h);
            _ = h.finish();
        }

        var timer = try std.time.Timer.start();
        i = 0;
        while (i < bench_iters) : (i += 1) {
            var h = ssz.Hasher.init(allocator);
            defer h.deinit();
            block.treeHashWith(&h);
            _ = h.finish();
        }
        const elapsed_ns = timer.read();
        const us_per_op = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(bench_iters)) / 1000.0;
        std.debug.print("hash_tree_root: {d:.1} us/op\n", .{us_per_op});

        // Verify HTR
        var h = ssz.Hasher.init(allocator);
        defer h.deinit();
        block.treeHashWith(&h);
        const got_htr = h.finish();

        if (!std.mem.eql(u8, &got_htr, &expected_htr)) {
            std.debug.print("HTR MISMATCH!\n", .{});
            std.debug.print("  expected: {s}\n", .{expected_htr_hex});
            std.debug.print("  got:      ", .{});
            for (got_htr) |b| {
                std.debug.print("{x:0>2}", .{b});
            }
            std.debug.print("\n", .{});
            return error.HtrMismatch;
        }
        std.debug.print("HTR verified OK\n", .{});
    }
}
