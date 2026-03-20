const std = @import("std");
const deneb = @import("deneb_ssz.zig");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = gpa.allocator();
    
    const data = try std.fs.cwd().readFileAlloc(allocator, "block-mainnet.ssz", 2 * 1024 * 1024);
    defer allocator.free(data);
    
    std.debug.print("Data size: {}\n", .{data.len});
    
    const block = try deneb.SignedBeaconBlock.fromSszBytes(data);
    
    // Check a known field
    std.debug.print("Slot (from message): {}\n", .{block.message.slot});
    std.debug.print("Proposer index: {}\n", .{block.message.proposer_index});
    
    // Marshal and compare
    const encoded = try block.toSszBytes(allocator);
    defer allocator.free(encoded);
    
    std.debug.print("Encoded size: {}\n", .{encoded.len});
    
    if (encoded.len != data.len) {
        std.debug.print("SIZE MISMATCH: {} vs {}\n", .{encoded.len, data.len});
        return;
    }
    
    var mismatches: usize = 0;
    var first_mismatch: usize = 0;
    for (0..data.len) |i| {
        if (encoded[i] != data[i]) {
            if (mismatches == 0) first_mismatch = i;
            mismatches += 1;
        }
    }
    
    if (mismatches > 0) {
        std.debug.print("BYTE MISMATCHES: {} (first at offset {})\n", .{mismatches, first_mismatch});
    } else {
        std.debug.print("Round-trip OK\n", .{});
    }
}
