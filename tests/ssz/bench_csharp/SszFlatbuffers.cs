// SSZ runtime support for flatssz-generated C# code.
//
// Provides Hasher for merkleization, HasherPool for reuse,
// and SszError for decode errors.
//
// Ported from the Rust reference implementation at
// rust/ssz_flatbuffers/src/lib.rs

using System;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Security.Cryptography;

namespace SszFlatbuffers
{
    // ---- Errors ----

    public class SszError : Exception
    {
        public SszError(string message) : base(message) { }

        public static SszError BufferTooSmall() => new SszError("ssz: buffer too small");
        public static SszError InvalidOffset() => new SszError("ssz: invalid offset");
        public static SszError InvalidBool() => new SszError("ssz: invalid bool value");
    }

    // ---- Zero hashes ----

    public static class ZeroHashes
    {
        private static byte[][] zeroHashes = new byte[65][];

        static ZeroHashes()
        {
            using var sha = SHA256.Create();
            zeroHashes[0] = new byte[32];
            var pair = new byte[64];
            for (int i = 0; i < 64; i++)
            {
                Buffer.BlockCopy(zeroHashes[i], 0, pair, 0, 32);
                Buffer.BlockCopy(zeroHashes[i], 0, pair, 32, 32);
                zeroHashes[i + 1] = sha.ComputeHash(pair);
            }
        }

        public static byte[] Get(int level)
        {
            return zeroHashes[level];
        }
    }

    // ---- Hasher ----

    public class Hasher
    {
        private List<byte> buf;
        private byte[] tmp = new byte[64];

        public Hasher()
        {
            buf = new List<byte>(8192);
        }

        public static Hasher New()
        {
            return new Hasher();
        }

        public void Reset()
        {
            buf.Clear();
        }

        public int Index()
        {
            return buf.Count;
        }

        /// <summary>
        /// Returns the last 32 bytes of the buffer as the hash root.
        /// </summary>
        public byte[] HashRoot()
        {
            var result = new byte[32];
            if (buf.Count >= 32)
            {
                for (int i = 0; i < 32; i++)
                    result[i] = buf[buf.Count - 32 + i];
            }
            return result;
        }

        // ---- Put methods (32-byte padded chunks) ----

        public void AppendBytes32(byte[] b)
        {
            for (int i = 0; i < b.Length; i++)
                buf.Add(b[i]);
            int rest = b.Length % 32;
            if (rest != 0)
            {
                int pad = 32 - rest;
                for (int i = 0; i < pad; i++)
                    buf.Add(0);
            }
        }

        public void PutUInt64(ulong v)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(tmp, 0, 8), v);
            Array.Clear(tmp, 8, 24);
            AddRange(tmp, 0, 32);
        }

        public void PutUInt32(uint v)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(tmp, 0, 4), v);
            Array.Clear(tmp, 4, 28);
            AddRange(tmp, 0, 32);
        }

        public void PutUInt16(ushort v)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(new Span<byte>(tmp, 0, 2), v);
            Array.Clear(tmp, 2, 30);
            AddRange(tmp, 0, 32);
        }

        public void PutUInt8(byte v)
        {
            tmp[0] = v;
            Array.Clear(tmp, 1, 31);
            AddRange(tmp, 0, 32);
        }

        public void PutBool(bool v)
        {
            Array.Clear(tmp, 0, 32);
            if (v) tmp[0] = 1;
            AddRange(tmp, 0, 32);
        }

        public void PutBytes(byte[] b)
        {
            if (b.Length <= 32)
            {
                AppendBytes32(b);
                return;
            }
            int idx = Index();
            AppendBytes32(b);
            Merkleize(idx);
        }

        public void PutBitlist(byte[] bb, ulong maxSize)
        {
            ParseBitlist(bb, out byte[] bitlist, out ulong size);
            int idx = Index();
            AppendBytes32(bitlist);
            MerkleizeWithMixin(idx, size, (maxSize + 255) / 256);
        }

        public void PutZeroHash()
        {
            for (int i = 0; i < 32; i++)
                buf.Add(0);
        }

        // ---- Append methods (no padding) ----

        public void AppendBool(bool v)
        {
            buf.Add(v ? (byte)1 : (byte)0);
        }

        public void AppendUInt8(byte v)
        {
            buf.Add(v);
        }

        public void AppendUInt16(ushort v)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(new Span<byte>(tmp, 0, 2), v);
            buf.Add(tmp[0]);
            buf.Add(tmp[1]);
        }

        public void AppendUInt32(uint v)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(tmp, 0, 4), v);
            for (int i = 0; i < 4; i++)
                buf.Add(tmp[i]);
        }

        public void AppendUInt64(ulong v)
        {
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(tmp, 0, 8), v);
            for (int i = 0; i < 8; i++)
                buf.Add(tmp[i]);
        }

        public void FillUpTo32()
        {
            int rest = buf.Count % 32;
            if (rest != 0)
            {
                int pad = 32 - rest;
                for (int i = 0; i < pad; i++)
                    buf.Add(0);
            }
        }

        // ---- Aliases for codegen compatibility ----

        public void PutUint64(ulong v) => PutUInt64(v);
        public void PutUint32(uint v) => PutUInt32(v);
        public void PutUint16(ushort v) => PutUInt16(v);
        public void PutUint8(byte v) => PutUInt8(v);
        public void AppendUint64(ulong v) => AppendUInt64(v);
        public void AppendUint32(uint v) => AppendUInt32(v);
        public void AppendUint16(ushort v) => AppendUInt16(v);
        public void AppendUint8(byte v) => AppendUInt8(v);

        // ---- Merkleization ----

        public void Merkleize(int idx)
        {
            MerkleizeInner(idx, 0);
        }

        public void MerkleizeWithMixin(int idx, ulong num, ulong limit)
        {
            FillUpTo32();
            MerkleizeInner(idx, limit);

            // Mix in length: hash(root || encode(num))
            CopyFromBuf(idx, tmp, 0, 32);
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(tmp, 32, 8), num);
            Array.Clear(tmp, 40, 24);

            byte[] hash;
            using (var sha = SHA256.Create())
            {
                hash = sha.ComputeHash(tmp);
            }
            SetBufRange(idx, hash, 0, 32);
        }

        public void MerkleizeProgressive(int idx)
        {
            FillUpTo32();
            int inputLen = buf.Count - idx;
            int count = inputLen / 32;

            if (count == 0)
            {
                buf.RemoveRange(idx, buf.Count - idx);
                AddRange(ZeroHashes.Get(0), 0, 32);
                return;
            }
            if (count == 1)
            {
                // Single chunk: already a 32-byte root
                if (buf.Count > idx + 32)
                    buf.RemoveRange(idx + 32, buf.Count - (idx + 32));
                return;
            }

            // EIP-7916 progressive subtree merkleization.
            // Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
            var subtreeSizes = new List<int>();
            {
                int remaining = count;
                int first = Math.Min(1, remaining);
                if (first > 0)
                {
                    subtreeSizes.Add(first);
                    remaining -= first;
                }
                int sz = 4;
                while (remaining > 0)
                {
                    int take = Math.Min(sz, remaining);
                    subtreeSizes.Add(take);
                    remaining -= take;
                    // saturating multiply
                    sz = (sz > int.MaxValue / 4) ? int.MaxValue : sz * 4;
                }
            }

            // Compute root for each subtree via MerkleizeInner
            var subtreeRoots = new List<byte[]>(subtreeSizes.Count);
            int chunkOffset = idx;
            int expectedCap = 1;
            for (int s = 0; s < subtreeSizes.Count; s++)
            {
                int sz = subtreeSizes[s];
                var tmpHasher = new Hasher();
                for (int i = 0; i < sz * 32; i++)
                    tmpHasher.buf.Add(buf[chunkOffset + i]);
                tmpHasher.MerkleizeInner(0, (ulong)expectedCap);
                var root = new byte[32];
                for (int i = 0; i < 32; i++)
                    root[i] = tmpHasher.buf[i];
                subtreeRoots.Add(root);
                chunkOffset += sz * 32;
                expectedCap = (expectedCap > int.MaxValue / 4) ? int.MaxValue : expectedCap * 4;
            }

            // Right-fold: hash(root_0, hash(root_1, ... hash(root_n-1, zero_hash)))
            var acc = new byte[32];
            Buffer.BlockCopy(ZeroHashes.Get(0), 0, acc, 0, 32);

            using (var sha = SHA256.Create())
            {
                for (int i = subtreeRoots.Count - 1; i >= 0; i--)
                {
                    Buffer.BlockCopy(subtreeRoots[i], 0, tmp, 0, 32);
                    Buffer.BlockCopy(acc, 0, tmp, 32, 32);
                    var hash = sha.ComputeHash(tmp);
                    Buffer.BlockCopy(hash, 0, acc, 0, 32);
                }
            }

            buf.RemoveRange(idx, buf.Count - idx);
            AddRange(acc, 0, 32);
        }

        public void MerkleizeProgressiveWithMixin(int idx, ulong num)
        {
            MerkleizeProgressive(idx);

            // Mix in length: hash(root || encode(num))
            CopyFromBuf(idx, tmp, 0, 32);
            BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(tmp, 32, 8), num);
            Array.Clear(tmp, 40, 24);

            byte[] hash;
            using (var sha = SHA256.Create())
            {
                hash = sha.ComputeHash(tmp);
            }
            SetBufRange(idx, hash, 0, 32);
        }

        public void MerkleizeProgressiveWithActiveFields(int idx, byte[] activeFields)
        {
            MerkleizeProgressive(idx);

            // Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
            CopyFromBuf(idx, tmp, 0, 32);
            Array.Clear(tmp, 32, 32);
            int copyLen = Math.Min(activeFields.Length, 32);
            Buffer.BlockCopy(activeFields, 0, tmp, 32, copyLen);

            byte[] hash;
            using (var sha = SHA256.Create())
            {
                hash = sha.ComputeHash(tmp);
            }
            SetBufRange(idx, hash, 0, 32);
        }

        // ---- Internal merkleization ----

        private void MerkleizeInner(int idx, ulong limit)
        {
            int inputLen = buf.Count - idx;
            ulong count = (ulong)((inputLen + 31) / 32);
            if (limit == 0)
                limit = count;

            if (limit == 0)
            {
                buf.RemoveRange(idx, buf.Count - idx);
                AddRange(ZeroHashes.Get(0), 0, 32);
                return;
            }
            if (limit == 1)
            {
                if (count >= 1 && inputLen >= 32)
                {
                    if (buf.Count > idx + 32)
                        buf.RemoveRange(idx + 32, buf.Count - (idx + 32));
                }
                else
                {
                    buf.RemoveRange(idx, buf.Count - idx);
                    AddRange(ZeroHashes.Get(0), 0, 32);
                }
                return;
            }

            int depth = GetDepth(limit);
            if (inputLen == 0)
            {
                buf.RemoveRange(idx, buf.Count - idx);
                AddRange(ZeroHashes.Get(depth), 0, 32);
                return;
            }

            // Pad to 32-byte alignment
            int rest = (buf.Count - idx) % 32;
            if (rest != 0)
            {
                int pad = 32 - rest;
                for (int i = 0; i < pad; i++)
                    buf.Add(0);
            }

            using var sha = SHA256.Create();

            // In-place layer-by-layer hashing
            for (int i = 0; i < depth; i++)
            {
                int layerLen = (buf.Count - idx) / 32;

                if (layerLen % 2 == 1)
                {
                    // Pad with zero hash at this level
                    AddRange(ZeroHashes.Get(i), 0, 32);
                }

                int pairs = (buf.Count - idx) / 64;

                // Hash each pair in-place
                var pairBuf = new byte[64];
                var outBuf = new byte[pairs * 32];
                for (int p = 0; p < pairs; p++)
                {
                    CopyFromBuf(idx + p * 64, pairBuf, 0, 64);
                    byte[] h = sha.ComputeHash(pairBuf);
                    Buffer.BlockCopy(h, 0, outBuf, p * 32, 32);
                }

                buf.RemoveRange(idx, buf.Count - idx);
                AddRange(outBuf, 0, pairs * 32);
            }
        }

        // ---- Helpers ----

        /// <summary>
        /// Copy bytes from buf[srcOffset..] into dst[dstOffset..dstOffset+length].
        /// </summary>
        private void CopyFromBuf(int srcOffset, byte[] dst, int dstOffset, int length)
        {
            for (int i = 0; i < length; i++)
                dst[dstOffset + i] = buf[srcOffset + i];
        }

        /// <summary>
        /// Copy bytes from src[srcOffset..srcOffset+length] into buf starting at bufOffset.
        /// </summary>
        private void SetBufRange(int bufOffset, byte[] src, int srcOffset, int length)
        {
            for (int i = 0; i < length; i++)
                buf[bufOffset + i] = src[srcOffset + i];
        }

        /// <summary>
        /// Append bytes from src[offset..offset+length] to buf.
        /// </summary>
        private void AddRange(byte[] src, int offset, int length)
        {
            for (int i = 0; i < length; i++)
                buf.Add(src[offset + i]);
        }

        // ---- Static helpers ----

        private static int GetDepth(ulong d)
        {
            if (d <= 1) return 0;
            ulong p = NextPowerOfTwo(d);
            // 63 - leading zeros
            return 63 - LeadingZeros(p);
        }

        private static ulong NextPowerOfTwo(ulong v)
        {
            if (v == 0) return 1;
            v--;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v |= v >> 32;
            return v + 1;
        }

        private static int LeadingZeros(ulong v)
        {
            if (v == 0) return 64;
            int n = 0;
            if (v <= 0x00000000FFFFFFFF) { n += 32; v <<= 32; }
            if (v <= 0x0000FFFFFFFFFFFF) { n += 16; v <<= 16; }
            if (v <= 0x00FFFFFFFFFFFFFF) { n += 8;  v <<= 8;  }
            if (v <= 0x0FFFFFFFFFFFFFFF) { n += 4;  v <<= 4;  }
            if (v <= 0x3FFFFFFFFFFFFFFF) { n += 2;  v <<= 2;  }
            if (v <= 0x7FFFFFFFFFFFFFFF) { n += 1; }
            return n;
        }

        private static void ParseBitlist(byte[] src, out byte[] bitlist, out ulong size)
        {
            if (src == null || src.Length == 0)
            {
                bitlist = Array.Empty<byte>();
                size = 0;
                return;
            }
            byte last = src[src.Length - 1];
            if (last == 0)
            {
                bitlist = Array.Empty<byte>();
                size = 0;
                return;
            }
            int msb = 7 - LeadingZerosByte(last);
            size = (ulong)(8 * (src.Length - 1)) + (ulong)msb;

            // Copy and clear the sentinel bit
            bitlist = new byte[src.Length];
            Buffer.BlockCopy(src, 0, bitlist, 0, src.Length);
            bitlist[bitlist.Length - 1] &= (byte)(~(1 << msb));

            // Trim trailing zero bytes
            int end = bitlist.Length;
            while (end > 0 && bitlist[end - 1] == 0)
                end--;
            if (end < bitlist.Length)
            {
                var trimmed = new byte[end];
                Buffer.BlockCopy(bitlist, 0, trimmed, 0, end);
                bitlist = trimmed;
            }
        }

        private static int LeadingZerosByte(byte v)
        {
            if (v == 0) return 8;
            int n = 0;
            if (v <= 0x0F) { n += 4; v <<= 4; }
            if (v <= 0x3F) { n += 2; v <<= 2; }
            if (v <= 0x7F) { n += 1; }
            return n;
        }
    }

    // ---- HasherPool ----

    public static class HasherPool
    {
        private static readonly ConcurrentBag<Hasher> pool = new ConcurrentBag<Hasher>();
        private const int MaxPoolSize = 16;

        public static Hasher Get()
        {
            if (pool.TryTake(out Hasher h))
                return h;
            return new Hasher();
        }

        public static void Put(Hasher h)
        {
            h.Reset();
            if (pool.Count < MaxPoolSize)
                pool.Add(h);
        }
    }
}
