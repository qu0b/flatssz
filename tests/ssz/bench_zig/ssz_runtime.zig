//! SSZ runtime support for flatssz-generated Zig code.
//!
//! Provides `Hasher` for merkleization, `HasherPool` for reuse,
//! and `SszError` for decode errors.
//!
//! Uses `std.crypto.hash.sha2.Sha256` for SHA-256 hashing.

const std = @import("std");
const Sha256 = std.crypto.hash.sha2.Sha256;
const Allocator = std.mem.Allocator;

// ---- Errors ----

pub const SszError = error{
    BufferTooSmall,
    InvalidOffset,
    InvalidBool,
};

// ---- Zero hashes ----

/// Precomputed zero hashes: zero_hashes[0] = [0; 32], zero_hashes[i] = sha256(zero_hashes[i-1] ++ zero_hashes[i-1]).
pub const zero_hashes: [65][32]u8 = blk: {
    @setEvalBranchQuota(10_000_000);
    var zh: [65][32]u8 = undefined;
    zh[0] = [_]u8{0} ** 32;
    for (0..64) |i| {
        var pair: [64]u8 = undefined;
        @memcpy(pair[0..32], &zh[i]);
        @memcpy(pair[32..64], &zh[i]);
        zh[i + 1] = sha256Comptime(&pair);
    }
    break :blk zh;
};

fn sha256Comptime(data: []const u8) [32]u8 {
    @setEvalBranchQuota(100_000);
    var h = Sha256.init(.{});
    h.update(data);
    return h.finalResult();
}

// ---- Helper functions ----

fn getDepth(d: u64) u8 {
    if (d <= 1) return 0;
    const i = std.math.ceilPowerOfTwo(u64, d) catch unreachable;
    return @intCast(63 - @clz(i));
}

fn parseBitlist(allocator: Allocator, buf: []const u8) !struct { data: std.ArrayList(u8), size: u64 } {
    if (buf.len == 0) {
        return .{ .data = std.ArrayList(u8).init(allocator), .size = 0 };
    }
    const last = buf[buf.len - 1];
    if (last == 0) {
        return .{ .data = std.ArrayList(u8).init(allocator), .size = 0 };
    }
    const msb: u8 = 7 - @as(u8, @intCast(@clz(last)));
    const size: u64 = 8 * (@as(u64, buf.len) - 1) + @as(u64, msb);
    var dst = std.ArrayList(u8).init(allocator);
    try dst.appendSlice(buf);
    dst.items[dst.items.len - 1] &= ~(@as(u8, 1) << @intCast(msb));
    while (dst.items.len > 0 and dst.items[dst.items.len - 1] == 0) {
        _ = dst.pop();
    }
    return .{ .data = dst, .size = size };
}

// ---- Hasher ----

/// Buffer-based SSZ merkleization engine.
///
/// Uses SHA-256 for hashing of merkle tree layers
/// and in-place buffer operations for minimal allocations.
pub const Hasher = struct {
    buf: std.ArrayList(u8),
    tmp: [64]u8,
    allocator: Allocator,

    const default_allocator = std.heap.page_allocator;

    pub fn init() Hasher {
        return initWithAllocator(default_allocator);
    }

    pub fn initWithAllocator(allocator: Allocator) Hasher {
        return .{
            .buf = std.ArrayList(u8).init(allocator),
            .tmp = [_]u8{0} ** 64,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *Hasher) void {
        self.buf.deinit();
    }

    pub fn reset(self: *Hasher) void {
        self.buf.clearRetainingCapacity();
    }

    pub fn index(self: *const Hasher) usize {
        return self.buf.items.len;
    }

    pub fn finish(self: *const Hasher) [32]u8 {
        var out = [_]u8{0} ** 32;
        if (self.buf.items.len >= 32) {
            @memcpy(&out, self.buf.items[self.buf.items.len - 32 ..]);
        }
        return out;
    }

    // ---- Put methods (32-byte padded chunks) ----

    pub fn appendBytes32(self: *Hasher, b: []const u8) void {
        self.buf.appendSlice(b) catch unreachable;
        const rest = b.len % 32;
        if (rest != 0) {
            const zeros = [_]u8{0} ** 32;
            self.buf.appendSlice(zeros[0 .. 32 - rest]) catch unreachable;
        }
    }

    pub fn putU64(self: *Hasher, v: u64) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u64, v));
        @memcpy(self.tmp[0..8], &bytes);
        @memset(self.tmp[8..32], 0);
        self.buf.appendSlice(self.tmp[0..32]) catch unreachable;
    }

    pub fn putU32(self: *Hasher, v: u32) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u32, v));
        @memcpy(self.tmp[0..4], &bytes);
        @memset(self.tmp[4..32], 0);
        self.buf.appendSlice(self.tmp[0..32]) catch unreachable;
    }

    pub fn putU16(self: *Hasher, v: u16) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u16, v));
        @memcpy(self.tmp[0..2], &bytes);
        @memset(self.tmp[2..32], 0);
        self.buf.appendSlice(self.tmp[0..32]) catch unreachable;
    }

    pub fn putU8(self: *Hasher, v: u8) void {
        self.tmp[0] = v;
        @memset(self.tmp[1..32], 0);
        self.buf.appendSlice(self.tmp[0..32]) catch unreachable;
    }

    pub fn putBool(self: *Hasher, v: bool) void {
        @memset(self.tmp[0..32], 0);
        if (v) {
            self.tmp[0] = 1;
        }
        self.buf.appendSlice(self.tmp[0..32]) catch unreachable;
    }

    pub fn putBytes(self: *Hasher, b: []const u8) void {
        if (b.len <= 32) {
            self.appendBytes32(b);
            return;
        }
        const idx = self.index();
        self.appendBytes32(b);
        self.merkleize(idx);
    }

    pub fn putBitlist(self: *Hasher, bb: []const u8, maxSize: u64) void {
        const result = parseBitlist(self.allocator, bb) catch unreachable;
        var bitlist = result.data;
        defer bitlist.deinit();
        const size = result.size;
        const idx = self.index();
        self.appendBytes32(bitlist.items);
        self.merkleizeWithMixin(idx, size, (maxSize + 255) / 256);
    }

    pub fn putZeroHash(self: *Hasher) void {
        const zeros = [_]u8{0} ** 32;
        self.buf.appendSlice(&zeros) catch unreachable;
    }

    // ---- Append methods (no padding) ----

    pub fn appendBool(self: *Hasher, v: bool) void {
        self.buf.append(if (v) @as(u8, 1) else @as(u8, 0)) catch unreachable;
    }

    pub fn appendU8(self: *Hasher, v: u8) void {
        self.buf.append(v) catch unreachable;
    }

    pub fn appendU16(self: *Hasher, v: u16) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u16, v));
        self.buf.appendSlice(&bytes) catch unreachable;
    }

    pub fn appendU32(self: *Hasher, v: u32) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u32, v));
        self.buf.appendSlice(&bytes) catch unreachable;
    }

    pub fn appendU64(self: *Hasher, v: u64) void {
        const bytes = std.mem.toBytes(std.mem.nativeToLittle(u64, v));
        self.buf.appendSlice(&bytes) catch unreachable;
    }

    pub fn fillUpTo32(self: *Hasher) void {
        const rest = self.buf.items.len % 32;
        if (rest != 0) {
            const zeros = [_]u8{0} ** 32;
            self.buf.appendSlice(zeros[0 .. 32 - rest]) catch unreachable;
        }
    }

    // ---- Aliases for codegen compatibility ----

    pub const putUint64 = putU64;
    pub const putUint32 = putU32;
    pub const putUint16 = putU16;
    pub const putUint8 = putU8;
    pub const appendUint64 = appendU64;
    pub const appendUint32 = appendU32;
    pub const appendUint16 = appendU16;
    pub const appendUint8 = appendU8;

    // ---- Merkleization ----

    pub fn merkleize(self: *Hasher, idx: usize) void {
        self.merkleizeInner(idx, 0);
    }

    pub fn merkleizeWithMixin(self: *Hasher, idx: usize, num: u64, limit: u64) void {
        self.fillUpTo32();
        self.merkleizeInner(idx, limit);

        // Mix in length: hash(root || encode(num))
        std.debug.assert(self.buf.items.len == idx + 32);
        @memcpy(self.tmp[0..32], self.buf.items[idx .. idx + 32]);
        const num_bytes = std.mem.toBytes(std.mem.nativeToLittle(u64, num));
        @memcpy(self.tmp[32..40], &num_bytes);
        @memset(self.tmp[40..64], 0);

        var hasher = Sha256.init(.{});
        hasher.update(&self.tmp);
        const hash = hasher.finalResult();
        @memcpy(self.buf.items[idx .. idx + 32], &hash);
    }

    pub fn merkleizeProgressive(self: *Hasher, idx: usize) void {
        self.fillUpTo32();
        const input_len = self.buf.items.len - idx;
        const count = input_len / 32;

        if (count == 0) {
            self.buf.shrinkRetainingCapacity(idx);
            self.buf.appendSlice(&zero_hashes[0]) catch unreachable;
            return;
        }
        if (count == 1) {
            // Single chunk: already a 32-byte root
            self.buf.shrinkRetainingCapacity(idx + 32);
            return;
        }

        // EIP-7916 progressive subtree merkleization.
        // Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
        // For each subtree: extract chunks, merkleize as binary tree (pad with
        // zero hashes), produce a 32-byte root.
        // Chain: hash(subtree_1_root, hash(subtree_2_root, hash(..., zero_hash)))

        var subtree_sizes = std.ArrayList(usize).init(self.allocator);
        defer subtree_sizes.deinit();
        {
            var remaining = count;
            const first = @min(@as(usize, 1), remaining);
            if (first > 0) {
                subtree_sizes.append(first) catch unreachable;
                remaining -= first;
            }
            var sz: usize = 4;
            while (remaining > 0) {
                const take = @min(sz, remaining);
                subtree_sizes.append(take) catch unreachable;
                remaining -= take;
                sz = std.math.mul(usize, sz, 4) catch std.math.maxInt(usize);
            }
        }

        // Compute root for each subtree via merkleizeInner
        var subtree_roots = std.ArrayList([32]u8).init(self.allocator);
        defer subtree_roots.deinit();
        var chunk_offset = idx;
        var expected_cap: usize = 1;
        for (subtree_sizes.items) |sz| {
            var tmp_hasher = Hasher.initWithAllocator(self.allocator);
            defer tmp_hasher.deinit();
            tmp_hasher.buf.appendSlice(
                self.buf.items[chunk_offset .. chunk_offset + sz * 32],
            ) catch unreachable;
            tmp_hasher.merkleizeInner(0, @as(u64, expected_cap));
            var root: [32]u8 = undefined;
            @memcpy(&root, tmp_hasher.buf.items[0..32]);
            subtree_roots.append(root) catch unreachable;
            chunk_offset += sz * 32;
            expected_cap = std.math.mul(usize, expected_cap, 4) catch std.math.maxInt(usize);
        }

        // Right-fold: hash(root_0, hash(root_1, hash(root_2, ... hash(root_n-1, zero_hash))))
        var acc: [32]u8 = zero_hashes[0];
        var i: usize = subtree_roots.items.len;
        while (i > 0) {
            i -= 1;
            const root = subtree_roots.items[i];
            @memcpy(self.tmp[0..32], &root);
            @memcpy(self.tmp[32..64], &acc);
            var sha = Sha256.init(.{});
            sha.update(&self.tmp);
            acc = sha.finalResult();
        }

        self.buf.shrinkRetainingCapacity(idx);
        self.buf.appendSlice(&acc) catch unreachable;
    }

    pub fn merkleizeProgressiveWithMixin(self: *Hasher, idx: usize, num: u64) void {
        self.merkleizeProgressive(idx);

        // Mix in length: hash(root || encode(num))
        std.debug.assert(self.buf.items.len == idx + 32);
        @memcpy(self.tmp[0..32], self.buf.items[idx .. idx + 32]);
        const num_bytes = std.mem.toBytes(std.mem.nativeToLittle(u64, num));
        @memcpy(self.tmp[32..40], &num_bytes);
        @memset(self.tmp[40..64], 0);
        var hasher = Sha256.init(.{});
        hasher.update(&self.tmp);
        const hash = hasher.finalResult();
        @memcpy(self.buf.items[idx .. idx + 32], &hash);
    }

    pub fn merkleizeProgressiveWithActiveFields(self: *Hasher, idx: usize, activeFields: []const u8) void {
        self.merkleizeProgressive(idx);

        // Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
        std.debug.assert(self.buf.items.len == idx + 32);
        @memcpy(self.tmp[0..32], self.buf.items[idx .. idx + 32]);
        @memset(self.tmp[32..64], 0);
        const copy_len = @min(activeFields.len, 32);
        @memcpy(self.tmp[32 .. 32 + copy_len], activeFields[0..copy_len]);
        var hasher = Sha256.init(.{});
        hasher.update(&self.tmp);
        const hash = hasher.finalResult();
        @memcpy(self.buf.items[idx .. idx + 32], &hash);
    }

    fn merkleizeInner(self: *Hasher, idx: usize, limit_in: u64) void {
        var limit = limit_in;
        const input_len = self.buf.items.len - idx;
        const count: u64 = @intCast((input_len + 31) / 32);
        if (limit == 0) {
            limit = count;
        }

        if (limit == 0) {
            self.buf.shrinkRetainingCapacity(idx);
            self.buf.appendSlice(&zero_hashes[0]) catch unreachable;
            return;
        }
        if (limit == 1) {
            if (count >= 1 and input_len >= 32) {
                self.buf.shrinkRetainingCapacity(idx + 32);
            } else {
                self.buf.shrinkRetainingCapacity(idx);
                self.buf.appendSlice(&zero_hashes[0]) catch unreachable;
            }
            return;
        }

        const depth = getDepth(limit);
        if (input_len == 0) {
            self.buf.shrinkRetainingCapacity(idx);
            self.buf.appendSlice(&zero_hashes[@as(usize, depth)]) catch unreachable;
            return;
        }

        // Pad to 32-byte alignment
        const rest = (self.buf.items.len - idx) % 32;
        if (rest != 0) {
            const zeros = [_]u8{0} ** 32;
            self.buf.appendSlice(zeros[0 .. 32 - rest]) catch unreachable;
        }

        // In-place layer-by-layer hashing
        var i: u8 = 0;
        while (i < depth) : (i += 1) {
            const layer_len = (self.buf.items.len - idx) / 32;

            if (layer_len % 2 == 1) {
                self.buf.appendSlice(&zero_hashes[@as(usize, i)]) catch unreachable;
            }

            const pairs = (self.buf.items.len - idx) / 64;

            // Hash each pair of 32-byte chunks
            var p: usize = 0;
            while (p < pairs) : (p += 1) {
                const pair_start = idx + p * 64;
                var sha = Sha256.init(.{});
                sha.update(self.buf.items[pair_start .. pair_start + 64]);
                const hash = sha.finalResult();
                @memcpy(self.buf.items[idx + p * 32 .. idx + p * 32 + 32], &hash);
            }
            self.buf.shrinkRetainingCapacity(idx + pairs * 32);
        }
    }
};

// ---- HasherPool ----

/// Thread-safe pool of `Hasher` instances for reuse.
/// Avoids repeated allocation/deallocation when hashing many objects.
pub const HasherPool = struct {
    const max_pool_size = 16;

    mutex: std.Thread.Mutex,
    pool: std.ArrayList(Hasher),
    allocator: Allocator,

    pub fn init(allocator: Allocator) HasherPool {
        return .{
            .mutex = .{},
            .pool = std.ArrayList(Hasher).init(allocator),
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *HasherPool) void {
        for (self.pool.items) |*h| {
            var hasher = h.*;
            hasher.deinit();
        }
        self.pool.deinit();
    }

    pub fn get(self: *HasherPool) Hasher {
        self.mutex.lock();
        defer self.mutex.unlock();
        if (self.pool.items.len > 0) {
            return self.pool.pop();
        }
        return Hasher.initWithAllocator(self.allocator);
    }

    pub fn put(self: *HasherPool, h: Hasher) void {
        var hasher = h;
        hasher.reset();
        self.mutex.lock();
        defer self.mutex.unlock();
        if (self.pool.items.len < max_pool_size) {
            self.pool.append(hasher) catch {
                hasher.deinit();
            };
        } else {
            hasher.deinit();
        }
    }
};

// ---- Tests ----

test "zero_hashes are computed correctly" {
    // zero_hashes[0] must be all zeros
    const expected_zero: [32]u8 = [_]u8{0} ** 32;
    try std.testing.expectEqual(expected_zero, zero_hashes[0]);

    // zero_hashes[1] = sha256(zero_hashes[0] ++ zero_hashes[0])
    var pair: [64]u8 = [_]u8{0} ** 64;
    var sha = Sha256.init(.{});
    sha.update(&pair);
    const expected_one = sha.finalResult();
    try std.testing.expectEqual(expected_one, zero_hashes[1]);

    // zero_hashes[2] = sha256(zero_hashes[1] ++ zero_hashes[1])
    @memcpy(pair[0..32], &zero_hashes[1]);
    @memcpy(pair[32..64], &zero_hashes[1]);
    sha = Sha256.init(.{});
    sha.update(&pair);
    const expected_two = sha.finalResult();
    try std.testing.expectEqual(expected_two, zero_hashes[2]);
}

test "getDepth" {
    try std.testing.expectEqual(@as(u8, 0), getDepth(0));
    try std.testing.expectEqual(@as(u8, 0), getDepth(1));
    try std.testing.expectEqual(@as(u8, 1), getDepth(2));
    try std.testing.expectEqual(@as(u8, 2), getDepth(3));
    try std.testing.expectEqual(@as(u8, 2), getDepth(4));
    try std.testing.expectEqual(@as(u8, 3), getDepth(5));
    try std.testing.expectEqual(@as(u8, 3), getDepth(8));
    try std.testing.expectEqual(@as(u8, 4), getDepth(9));
    try std.testing.expectEqual(@as(u8, 4), getDepth(16));
}

test "merkleize single chunk" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();
    h.putU64(42);
    h.merkleize(0);
    try std.testing.expectEqual(@as(usize, 32), h.buf.items.len);
}

test "merkleize deterministic" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();

    const idx1 = h.index();
    h.putU64(1);
    h.putU64(2);
    h.merkleize(idx1);
    const root1 = h.finish();

    h.reset();
    const idx2 = h.index();
    h.putU64(1);
    h.putU64(2);
    h.merkleize(idx2);
    const root2 = h.finish();

    try std.testing.expectEqual(root1, root2);
}

test "mixin" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();
    const idx = h.index();
    h.appendBytes32(&[_]u8{ 1, 1, 1, 1 });
    h.merkleizeWithMixin(idx, 4, 32);
    const root = h.finish();
    const all_zero: [32]u8 = [_]u8{0} ** 32;
    try std.testing.expect(!std.mem.eql(u8, &root, &all_zero));
}

test "hasher pool get and put" {
    var pool = HasherPool.init(std.testing.allocator);
    defer pool.deinit();

    var h = pool.get();
    h.putU64(123);
    try std.testing.expectEqual(@as(usize, 32), h.buf.items.len);
    pool.put(h);

    // Getting again should return a reset hasher
    const h2 = pool.get();
    defer pool.put(h2);
    try std.testing.expectEqual(@as(usize, 0), h2.buf.items.len);
}

test "parseBitlist" {
    // Empty
    {
        const result = try parseBitlist(std.testing.allocator, &[_]u8{});
        var data = result.data;
        defer data.deinit();
        try std.testing.expectEqual(@as(u64, 0), result.size);
        try std.testing.expectEqual(@as(usize, 0), data.items.len);
    }
    // Single byte 0b00001000 = 8 => msb=3, size=3, clear bit 3 => 0
    {
        const result = try parseBitlist(std.testing.allocator, &[_]u8{0b00001000});
        var data = result.data;
        defer data.deinit();
        try std.testing.expectEqual(@as(u64, 3), result.size);
    }
}

test "progressive merkleize" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();

    // 0 chunks => zero hash
    {
        const idx = h.index();
        h.merkleizeProgressive(idx);
        const root = h.finish();
        try std.testing.expectEqual(zero_hashes[0], root);
    }

    // 1 chunk => identity
    h.reset();
    {
        h.putU64(42);
        const expected = h.finish();
        const idx: usize = 0;
        h.merkleizeProgressive(idx);
        const root = h.finish();
        try std.testing.expectEqual(expected, root);
    }
}

test "progressive merkleize with mixin" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();
    const idx = h.index();
    h.putU64(1);
    h.putU64(2);
    h.merkleizeProgressiveWithMixin(idx, 2);
    const root = h.finish();
    const all_zero: [32]u8 = [_]u8{0} ** 32;
    try std.testing.expect(!std.mem.eql(u8, &root, &all_zero));
}

test "fillUpTo32" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();
    h.appendU8(0xFF);
    h.fillUpTo32();
    try std.testing.expectEqual(@as(usize, 32), h.buf.items.len);
    // Already aligned, should not grow
    h.fillUpTo32();
    try std.testing.expectEqual(@as(usize, 32), h.buf.items.len);
}

test "put methods produce 32-byte chunks" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();

    h.putBool(true);
    try std.testing.expectEqual(@as(usize, 32), h.buf.items.len);
    try std.testing.expectEqual(@as(u8, 1), h.buf.items[0]);

    h.putU8(0xAB);
    try std.testing.expectEqual(@as(usize, 64), h.buf.items.len);
    try std.testing.expectEqual(@as(u8, 0xAB), h.buf.items[32]);

    h.putU16(0x1234);
    try std.testing.expectEqual(@as(usize, 96), h.buf.items.len);
    try std.testing.expectEqual(@as(u8, 0x34), h.buf.items[64]);
    try std.testing.expectEqual(@as(u8, 0x12), h.buf.items[65]);

    h.putU32(0xDEADBEEF);
    try std.testing.expectEqual(@as(usize, 128), h.buf.items.len);

    h.putU64(0xCAFEBABE_12345678);
    try std.testing.expectEqual(@as(usize, 160), h.buf.items.len);
}

test "append methods do not pad" {
    var h = Hasher.initWithAllocator(std.testing.allocator);
    defer h.deinit();

    h.appendBool(false);
    try std.testing.expectEqual(@as(usize, 1), h.buf.items.len);

    h.appendU8(42);
    try std.testing.expectEqual(@as(usize, 2), h.buf.items.len);

    h.appendU16(1000);
    try std.testing.expectEqual(@as(usize, 4), h.buf.items.len);

    h.appendU32(100000);
    try std.testing.expectEqual(@as(usize, 8), h.buf.items.len);

    h.appendU64(999999999);
    try std.testing.expectEqual(@as(usize, 16), h.buf.items.len);
}
