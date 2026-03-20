const std = @import("std");
const ssz = @import("ssz_runtime.zig");
const deneb = @import("deneb_ssz.zig");
const Hasher = ssz.Hasher;
const SignedBeaconBlock = deneb.SignedBeaconBlock;

pub fn main() !void {
    ssz.initHashtree();
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = gpa.allocator();
    const data = try std.fs.cwd().readFileAlloc(allocator, "block-mainnet.ssz", 2 * 1024 * 1024);
    defer allocator.free(data);
    const meta_json = try std.fs.cwd().readFileAlloc(allocator, "block-mainnet-meta.json", 4096);
    defer allocator.free(meta_json);
    const htr_key = "\"htr\": \"";
    const start_idx = std.mem.indexOf(u8, meta_json, htr_key).? + htr_key.len;
    const end_idx = std.mem.indexOfPos(u8, meta_json, start_idx, "\"").?;
    const hex = meta_json[start_idx..end_idx];
    var expected: [32]u8 = undefined;
    for (0..32) |i| expected[i] = std.fmt.parseInt(u8, hex[i*2..i*2+2], 16) catch unreachable;

    std.debug.print("SSZ data: {} bytes\n", .{data.len});

    // Unmarshal (100K iterations — it's nearly free, zero-copy)
    {
        for (0..1000) |_| _ = try SignedBeaconBlock.fromSszBytes(data);
        var timer = try std.time.Timer.start();
        const N: usize = 1000000;
        for (0..N) |_| _ = try SignedBeaconBlock.fromSszBytes(data);
        const ns = timer.read();
        std.debug.print("unmarshal: {d:.1} ns/op\n", .{@as(f64, @floatFromInt(ns)) / @as(f64, @floatFromInt(N))});
    }

    // Marshal
    {
        const block = try SignedBeaconBlock.fromSszBytes(data);
        for (0..100) |_| { const b = try block.toSszBytes(allocator); allocator.free(b); }
        var timer = try std.time.Timer.start();
        const N: usize = 1000;
        for (0..N) |_| { const b = try block.toSszBytes(allocator); allocator.free(b); }
        const ns = timer.read();
        std.debug.print("marshal: {d:.0} us/op\n", .{@as(f64, @floatFromInt(ns)) / @as(f64, @floatFromInt(N)) / 1000.0});
    }

    // HTR
    {
        const block = try SignedBeaconBlock.fromSszBytes(data);
        for (0..50) |_| { var h = Hasher.initWithAllocator(allocator); block.message.treeHashWith(&h); _ = h.finish(); }
        var timer = try std.time.Timer.start();
        const N: usize = 500;
        for (0..N) |_| { var h = Hasher.initWithAllocator(allocator); block.message.treeHashWith(&h); _ = h.finish(); }
        const ns = timer.read();
        std.debug.print("hash_tree_root: {d:.0} us/op\n", .{@as(f64, @floatFromInt(ns)) / @as(f64, @floatFromInt(N)) / 1000.0});

        var h = Hasher.initWithAllocator(allocator); block.message.treeHashWith(&h);
        const got = h.finish();
        if (!std.mem.eql(u8, &got, &expected)) {
            std.debug.print("HTR MISMATCH\n", .{});
        } else {
            std.debug.print("HTR verified OK\n", .{});
        }
    }
}
