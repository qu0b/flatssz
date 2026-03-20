package ssz;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Deque;

/**
 * Buffer-based SSZ merkleization engine.
 *
 * <p>Uses {@code java.security.MessageDigest} SHA-256 for hashing.
 * Port of the Rust {@code ssz_flatbuffers} reference implementation.
 */
public class Hasher {

    // ---- Zero hashes (precomputed) ----

    private static final byte[][] ZERO_HASHES = new byte[65][32];

    static {
        // ZERO_HASHES[0] is 32 zero bytes (already initialized).
        byte[] pair = new byte[64];
        for (int i = 0; i < 64; i++) {
            System.arraycopy(ZERO_HASHES[i], 0, pair, 0, 32);
            System.arraycopy(ZERO_HASHES[i], 0, pair, 32, 32);
            ZERO_HASHES[i + 1] = sha256Static(pair);
        }
    }

    // ---- Instance fields ----

    private byte[] buf;
    private int len; // logical length of buf
    private byte[] tmp = new byte[64];

    public Hasher() {
        buf = new byte[8192];
        len = 0;
    }

    // ---- Core accessors ----

    /** Current write position (number of bytes in the buffer). */
    public int index() {
        return len;
    }

    /** Returns the last 32 bytes of the buffer as a new array. */
    public byte[] finish() {
        byte[] out = new byte[32];
        if (len >= 32) {
            System.arraycopy(buf, len - 32, out, 0, 32);
        }
        return out;
    }

    /** Clears the buffer. */
    public void reset() {
        len = 0;
    }

    // ---- Put methods (32-byte padded chunks) ----

    public void appendBytes32(byte[] b) {
        ensureCapacity(len + b.length + 32);
        System.arraycopy(b, 0, buf, len, b.length);
        len += b.length;
        int rest = b.length % 32;
        if (rest != 0) {
            int pad = 32 - rest;
            Arrays.fill(buf, len, len + pad, (byte) 0);
            len += pad;
        }
    }

    public void putU64(long v) {
        ensureCapacity(len + 32);
        writeLittleEndian64(buf, len, v);
        Arrays.fill(buf, len + 8, len + 32, (byte) 0);
        len += 32;
    }

    public void putU32(int v) {
        ensureCapacity(len + 32);
        writeLittleEndian32(buf, len, v);
        Arrays.fill(buf, len + 4, len + 32, (byte) 0);
        len += 32;
    }

    public void putU16(short v) {
        ensureCapacity(len + 32);
        buf[len] = (byte) (v & 0xFF);
        buf[len + 1] = (byte) ((v >> 8) & 0xFF);
        Arrays.fill(buf, len + 2, len + 32, (byte) 0);
        len += 32;
    }

    public void putU8(byte v) {
        ensureCapacity(len + 32);
        buf[len] = v;
        Arrays.fill(buf, len + 1, len + 32, (byte) 0);
        len += 32;
    }

    public void putBool(boolean v) {
        ensureCapacity(len + 32);
        Arrays.fill(buf, len, len + 32, (byte) 0);
        if (v) {
            buf[len] = 1;
        }
        len += 32;
    }

    public void putBytes(byte[] b) {
        if (b.length <= 32) {
            appendBytes32(b);
            return;
        }
        int idx = index();
        appendBytes32(b);
        merkleize(idx);
    }

    public void putBitlist(byte[] bb, long maxSize) {
        long[] result = parseBitlist(bb);
        byte[] bitlist = (result == null) ? new byte[0] : Arrays.copyOf(bb, (int) result[0]);
        long size = (result == null) ? 0 : result[1];

        if (result != null) {
            // Clear the sentinel bit in the bitlist copy
            // result[0] is the trimmed length, result[2] is lastIdx, result[3] is mask
            bitlist[(int) result[2]] &= (byte) result[3];
            // Trim trailing zeros
            int end = bitlist.length;
            while (end > 0 && bitlist[end - 1] == 0) {
                end--;
            }
            if (end < bitlist.length) {
                bitlist = Arrays.copyOf(bitlist, end);
            }
        }

        int idx = index();
        appendBytes32(bitlist);
        merkleizeWithMixin(idx, size, (maxSize + 255) / 256);
    }

    public void putZeroHash() {
        ensureCapacity(len + 32);
        Arrays.fill(buf, len, len + 32, (byte) 0);
        len += 32;
    }

    // ---- Append methods (no padding) ----

    public void appendBool(boolean v) {
        ensureCapacity(len + 1);
        buf[len++] = v ? (byte) 1 : (byte) 0;
    }

    public void appendU8(byte v) {
        ensureCapacity(len + 1);
        buf[len++] = v;
    }

    public void appendU16(short v) {
        ensureCapacity(len + 2);
        buf[len] = (byte) (v & 0xFF);
        buf[len + 1] = (byte) ((v >> 8) & 0xFF);
        len += 2;
    }

    public void appendU32(int v) {
        ensureCapacity(len + 4);
        writeLittleEndian32(buf, len, v);
        len += 4;
    }

    public void appendU64(long v) {
        ensureCapacity(len + 8);
        writeLittleEndian64(buf, len, v);
        len += 8;
    }

    public void fillUpTo32() {
        int rest = len % 32;
        if (rest != 0) {
            int pad = 32 - rest;
            ensureCapacity(len + pad);
            Arrays.fill(buf, len, len + pad, (byte) 0);
            len += pad;
        }
    }

    // ---- Merkleization ----

    public void merkleize(int idx) {
        merkleizeInner(idx, 0);
    }

    public void merkleizeWithMixin(int idx, long num, long limit) {
        fillUpTo32();
        merkleizeInner(idx, limit);

        // Mix in length: hash(root || encode(num))
        System.arraycopy(buf, idx, tmp, 0, 32);
        writeLittleEndian64(tmp, 32, num);
        Arrays.fill(tmp, 40, 64, (byte) 0);

        byte[] hash = sha256(tmp);
        System.arraycopy(hash, 0, buf, idx, 32);
    }

    public void merkleizeProgressive(int idx) {
        fillUpTo32();
        int inputLen = len - idx;
        int count = inputLen / 32;

        if (count == 0) {
            len = idx;
            ensureCapacity(len + 32);
            System.arraycopy(ZERO_HASHES[0], 0, buf, len, 32);
            len += 32;
            return;
        }
        if (count == 1) {
            len = idx + 32;
            return;
        }

        // EIP-7916 progressive subtree merkleization.
        // Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
        int[] subtreeSizes = computeSubtreeSizes(count);

        // Compute root for each subtree
        byte[][] subtreeRoots = new byte[subtreeSizes.length][];
        int chunkOffset = idx;
        long expectedCap = 1;
        for (int i = 0; i < subtreeSizes.length; i++) {
            int sz = subtreeSizes[i];
            Hasher tmpHasher = new Hasher();
            tmpHasher.ensureCapacity(sz * 32);
            System.arraycopy(buf, chunkOffset, tmpHasher.buf, 0, sz * 32);
            tmpHasher.len = sz * 32;
            tmpHasher.merkleizeInner(0, expectedCap);
            subtreeRoots[i] = new byte[32];
            System.arraycopy(tmpHasher.buf, 0, subtreeRoots[i], 0, 32);
            chunkOffset += sz * 32;
            expectedCap = saturatingMul4(expectedCap);
        }

        // Right-fold: hash(root_0, hash(root_1, ... hash(root_n-1, zero_hash)))
        byte[] acc = Arrays.copyOf(ZERO_HASHES[0], 32);
        for (int i = subtreeRoots.length - 1; i >= 0; i--) {
            System.arraycopy(subtreeRoots[i], 0, tmp, 0, 32);
            System.arraycopy(acc, 0, tmp, 32, 32);
            acc = sha256(tmp);
        }

        len = idx;
        ensureCapacity(len + 32);
        System.arraycopy(acc, 0, buf, len, 32);
        len += 32;
    }

    public void merkleizeProgressiveWithMixin(int idx, long num) {
        merkleizeProgressive(idx);

        // Mix in length: hash(root || encode(num))
        System.arraycopy(buf, idx, tmp, 0, 32);
        writeLittleEndian64(tmp, 32, num);
        Arrays.fill(tmp, 40, 64, (byte) 0);
        byte[] hash = sha256(tmp);
        System.arraycopy(hash, 0, buf, idx, 32);
    }

    public void merkleizeProgressiveWithActiveFields(int idx, byte[] activeFields) {
        merkleizeProgressive(idx);

        // Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
        System.arraycopy(buf, idx, tmp, 0, 32);
        Arrays.fill(tmp, 32, 64, (byte) 0);
        int copyLen = Math.min(activeFields.length, 32);
        System.arraycopy(activeFields, 0, tmp, 32, copyLen);
        byte[] hash = sha256(tmp);
        System.arraycopy(hash, 0, buf, idx, 32);
    }

    /** Convenience: merkleize from 0 and return the 32-byte root. */
    public byte[] hashRoot() {
        merkleize(0);
        return finish();
    }

    // ---- Private helpers ----

    private void merkleizeInner(int idx, long limit) {
        int inputLen = len - idx;
        long count = ((long) inputLen + 31) / 32;
        if (limit == 0) {
            limit = count;
        }

        if (limit == 0) {
            len = idx;
            ensureCapacity(len + 32);
            System.arraycopy(ZERO_HASHES[0], 0, buf, len, 32);
            len += 32;
            return;
        }
        if (limit == 1) {
            if (count >= 1 && inputLen >= 32) {
                len = idx + 32;
            } else {
                len = idx;
                ensureCapacity(len + 32);
                System.arraycopy(ZERO_HASHES[0], 0, buf, len, 32);
                len += 32;
            }
            return;
        }

        int depth = getDepth(limit);
        if (inputLen == 0) {
            len = idx;
            ensureCapacity(len + 32);
            System.arraycopy(ZERO_HASHES[depth], 0, buf, len, 32);
            len += 32;
            return;
        }

        // Pad to 32-byte alignment
        int rest = (len - idx) % 32;
        if (rest != 0) {
            int pad = 32 - rest;
            ensureCapacity(len + pad);
            Arrays.fill(buf, len, len + pad, (byte) 0);
            len += pad;
        }

        // Layer-by-layer hashing
        for (int i = 0; i < depth; i++) {
            int layerLen = (len - idx) / 32;

            if (layerLen % 2 == 1) {
                ensureCapacity(len + 32);
                System.arraycopy(ZERO_HASHES[i], 0, buf, len, 32);
                len += 32;
            }

            int pairs = (len - idx) / 64;
            // Hash each pair in place
            for (int p = 0; p < pairs; p++) {
                byte[] hash = sha256(buf, idx + p * 64, 64);
                System.arraycopy(hash, 0, buf, idx + p * 32, 32);
            }
            len = idx + pairs * 32;
        }
    }

    private void ensureCapacity(int required) {
        if (required <= buf.length) {
            return;
        }
        int newCap = buf.length;
        while (newCap < required) {
            newCap = newCap * 2;
        }
        buf = Arrays.copyOf(buf, newCap);
    }

    private byte[] sha256(byte[] data) {
        return sha256(data, 0, data.length);
    }

    private byte[] sha256(byte[] data, int offset, int length) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            md.update(data, offset, length);
            return md.digest();
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("SHA-256 not available", e);
        }
    }

    private static byte[] sha256Static(byte[] data) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return md.digest(data);
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("SHA-256 not available", e);
        }
    }

    private static int getDepth(long d) {
        if (d <= 1) {
            return 0;
        }
        long p = Long.highestOneBit(d);
        if (p < d) {
            p <<= 1;
        }
        return 63 - Long.numberOfLeadingZeros(p);
    }

    private static void writeLittleEndian64(byte[] dst, int off, long v) {
        dst[off] = (byte) v;
        dst[off + 1] = (byte) (v >>> 8);
        dst[off + 2] = (byte) (v >>> 16);
        dst[off + 3] = (byte) (v >>> 24);
        dst[off + 4] = (byte) (v >>> 32);
        dst[off + 5] = (byte) (v >>> 40);
        dst[off + 6] = (byte) (v >>> 48);
        dst[off + 7] = (byte) (v >>> 56);
    }

    private static void writeLittleEndian32(byte[] dst, int off, int v) {
        dst[off] = (byte) v;
        dst[off + 1] = (byte) (v >>> 8);
        dst[off + 2] = (byte) (v >>> 16);
        dst[off + 3] = (byte) (v >>> 24);
    }

    /**
     * Parses a bitlist by finding and removing the sentinel bit.
     * Returns null if empty or last byte is zero.
     * Otherwise returns {originalLength, bitCount, lastIdx, clearMask}.
     */
    private static long[] parseBitlist(byte[] buf) {
        if (buf.length == 0) {
            return null;
        }
        byte last = buf[buf.length - 1];
        if (last == 0) {
            return null;
        }
        int msb = 7 - Integer.numberOfLeadingZeros(last & 0xFF) + 24;
        // msb is the bit position of the highest set bit in `last` (0..7)
        msb = 31 - Integer.numberOfLeadingZeros(last & 0xFF);
        long size = 8L * (buf.length - 1) + msb;
        int lastIdx = buf.length - 1;
        long clearMask = ~(1 << msb) & 0xFF;
        return new long[]{buf.length, size, lastIdx, clearMask};
    }

    private static int[] computeSubtreeSizes(int count) {
        // Subtree sizes: 1, 4, 16, 64, ... (1, 4^1, 4^2, ...)
        int remaining = count;
        // Count how many subtrees we need
        int numSubtrees = 0;
        int r = remaining;
        int first = Math.min(1, r);
        if (first > 0) {
            numSubtrees++;
            r -= first;
        }
        long sz = 4;
        while (r > 0) {
            int take = (int) Math.min(sz, r);
            numSubtrees++;
            r -= take;
            sz = saturatingMul4(sz);
        }

        int[] sizes = new int[numSubtrees];
        int i = 0;
        first = Math.min(1, remaining);
        if (first > 0) {
            sizes[i++] = first;
            remaining -= first;
        }
        sz = 4;
        while (remaining > 0) {
            int take = (int) Math.min(sz, remaining);
            sizes[i++] = take;
            remaining -= take;
            sz = saturatingMul4(sz);
        }
        return sizes;
    }

    private static long saturatingMul4(long v) {
        if (v > Long.MAX_VALUE / 4) {
            return Long.MAX_VALUE;
        }
        return v * 4;
    }

    // ---- HasherPool ----

    /**
     * Simple pool for reusing {@link Hasher} instances.
     */
    public static class HasherPool {
        private static final int MAX_POOL_SIZE = 16;
        private static final Deque<Hasher> pool = new ArrayDeque<>();

        public static Hasher get() {
            synchronized (pool) {
                Hasher h = pool.pollLast();
                if (h != null) {
                    return h;
                }
            }
            return new Hasher();
        }

        public static void put(Hasher h) {
            h.reset();
            synchronized (pool) {
                if (pool.size() < MAX_POOL_SIZE) {
                    pool.addLast(h);
                }
            }
        }
    }
}
