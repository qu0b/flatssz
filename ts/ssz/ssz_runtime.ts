/**
 * SSZ runtime support for flatssz-generated TypeScript code.
 *
 * Provides {@link Hasher} for merkleization, {@link HasherPool} for reuse,
 * and {@link SszError} for decode errors.
 *
 * Uses a pure-TypeScript SHA-256 implementation for maximum portability
 * (no Node.js or Web Crypto dependency).
 */

// ---- Errors ----

export class SszError extends Error {
  private constructor(message: string) {
    super(message);
    this.name = "SszError";
  }

  static BufferTooSmall(): SszError {
    return new SszError("ssz: buffer too small");
  }
  static InvalidOffset(): SszError {
    return new SszError("ssz: invalid offset");
  }
  static InvalidBool(): SszError {
    return new SszError("ssz: invalid bool value");
  }
}

// ---- Pure TypeScript SHA-256 ----

const K: Uint32Array = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function sha256(data: Uint8Array): Uint8Array {
  // Pre-processing: pad message
  const bitLen = data.length * 8;
  // message + 1 byte (0x80) + padding + 8 bytes (length)
  const padded = new Uint8Array(
    Math.ceil((data.length + 9) / 64) * 64
  );
  padded.set(data);
  padded[data.length] = 0x80;
  // Big-endian 64-bit length at end
  const dv = new DataView(padded.buffer, padded.byteOffset, padded.byteLength);
  // We only support messages < 2^32 bits, so high 32 bits are 0
  dv.setUint32(padded.length - 4, bitLen, false);

  let h0 = 0x6a09e667 | 0;
  let h1 = 0xbb67ae85 | 0;
  let h2 = 0x3c6ef372 | 0;
  let h3 = 0xa54ff53a | 0;
  let h4 = 0x510e527f | 0;
  let h5 = 0x9b05688c | 0;
  let h6 = 0x1f83d9ab | 0;
  let h7 = 0x5be0cd19 | 0;

  const w = new Uint32Array(64);

  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) {
      w[i] = dv.getUint32(off + i * 4, false);
    }
    for (let i = 16; i < 64; i++) {
      const s0 =
        (((w[i - 15] >>> 7) | (w[i - 15] << 25)) ^
          ((w[i - 15] >>> 18) | (w[i - 15] << 14)) ^
          (w[i - 15] >>> 3)) >>>
        0;
      const s1 =
        (((w[i - 2] >>> 17) | (w[i - 2] << 15)) ^
          ((w[i - 2] >>> 19) | (w[i - 2] << 13)) ^
          (w[i - 2] >>> 10)) >>>
        0;
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }

    let a = h0,
      b = h1,
      c = h2,
      d = h3,
      e = h4,
      f = h5,
      g = h6,
      h = h7;

    for (let i = 0; i < 64; i++) {
      const S1 =
        (((e >>> 6) | (e << 26)) ^
          ((e >>> 11) | (e << 21)) ^
          ((e >>> 25) | (e << 7))) >>>
        0;
      const ch = ((e & f) ^ (~e & g)) >>> 0;
      const temp1 = (h + S1 + ch + K[i] + w[i]) | 0;
      const S0 =
        (((a >>> 2) | (a << 30)) ^
          ((a >>> 13) | (a << 19)) ^
          ((a >>> 22) | (a << 10))) >>>
        0;
      const maj = ((a & b) ^ (a & c) ^ (b & c)) >>> 0;
      const temp2 = (S0 + maj) | 0;

      h = g;
      g = f;
      f = e;
      e = (d + temp1) | 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) | 0;
    }

    h0 = (h0 + a) | 0;
    h1 = (h1 + b) | 0;
    h2 = (h2 + c) | 0;
    h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0;
    h5 = (h5 + f) | 0;
    h6 = (h6 + g) | 0;
    h7 = (h7 + h) | 0;
  }

  const out = new Uint8Array(32);
  const outView = new DataView(out.buffer);
  outView.setUint32(0, h0, false);
  outView.setUint32(4, h1, false);
  outView.setUint32(8, h2, false);
  outView.setUint32(12, h3, false);
  outView.setUint32(16, h4, false);
  outView.setUint32(20, h5, false);
  outView.setUint32(24, h6, false);
  outView.setUint32(28, h7, false);
  return out;
}

/**
 * Hash `count` consecutive 64-byte pairs from `input` into `output`.
 * Each pair of 32-byte chunks is SHA-256 hashed to produce one 32-byte result.
 */
function hashPairs(
  output: Uint8Array,
  input: Uint8Array,
  count: number
): void {
  for (let i = 0; i < count; i++) {
    const pair = input.subarray(i * 64, i * 64 + 64);
    const h = sha256(pair);
    output.set(h, i * 32);
  }
}

// ---- Zero hashes ----

function computeZeroHashes(): Uint8Array[] {
  const zh: Uint8Array[] = new Array(65);
  zh[0] = new Uint8Array(32); // all zeros
  const pair = new Uint8Array(64);
  for (let i = 0; i < 64; i++) {
    pair.set(zh[i], 0);
    pair.set(zh[i], 32);
    zh[i + 1] = sha256(pair);
  }
  return zh;
}

let _zeroHashes: Uint8Array[] | null = null;

function zeroHashes(): Uint8Array[] {
  if (_zeroHashes === null) {
    _zeroHashes = computeZeroHashes();
  }
  return _zeroHashes;
}

// ---- Utilities ----

function getDepth(d: number): number {
  if (d <= 1) return 0;
  // next power of two
  let v = d - 1;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v += 1;
  // log2
  return 31 - Math.clz32(v);
}

function parseBitlist(buf: Uint8Array): { data: Uint8Array; size: number } {
  if (buf.length === 0) {
    return { data: new Uint8Array(0), size: 0 };
  }
  const last = buf[buf.length - 1];
  if (last === 0) {
    return { data: new Uint8Array(0), size: 0 };
  }
  const msb = 7 - Math.clz32(last) + 24; // clz32 counts from 32-bit, last is 8-bit
  // Actually: Math.clz32(last) counts leading zeros of a 32-bit int.
  // For an 8-bit value, the bit position of the highest set bit is:
  //   31 - Math.clz32(last)
  // But we want it as a position within the byte (0-7):
  const bitPos = 31 - Math.clz32(last); // position in 32-bit
  // That's the same as the position in the byte since last < 256
  const msbInByte = bitPos; // 0..7

  const size = 8 * (buf.length - 1) + msbInByte;

  const dst = new Uint8Array(buf);
  dst[dst.length - 1] &= ~(1 << msbInByte);

  // Trim trailing zeros
  let end = dst.length;
  while (end > 0 && dst[end - 1] === 0) {
    end--;
  }
  return { data: dst.subarray(0, end), size };
}

// ---- Hasher ----

const INITIAL_CAPACITY = 8192;

export class Hasher {
  /** Growable buffer backed by an ArrayBuffer. */
  buf: Uint8Array;
  private _len: number;
  private _out: Uint8Array;
  private _tmp: Uint8Array;

  constructor() {
    this.buf = new Uint8Array(INITIAL_CAPACITY);
    this._len = 0;
    this._out = new Uint8Array(4096);
    this._tmp = new Uint8Array(64);
  }

  /** Current position in the buffer. */
  index(): number {
    return this._len;
  }

  /** Return the last 32 bytes of the buffer as a new Uint8Array. */
  finish(): Uint8Array {
    const out = new Uint8Array(32);
    if (this._len >= 32) {
      out.set(this.buf.subarray(this._len - 32, this._len));
    }
    return out;
  }

  reset(): void {
    this._len = 0;
  }

  // ---- Internal buffer management ----

  private _ensureCapacity(needed: number): void {
    const required = this._len + needed;
    if (required <= this.buf.length) return;
    let newCap = this.buf.length;
    while (newCap < required) {
      newCap *= 2;
    }
    const newBuf = new Uint8Array(newCap);
    newBuf.set(this.buf.subarray(0, this._len));
    this.buf = newBuf;
  }

  private _push(byte: number): void {
    this._ensureCapacity(1);
    this.buf[this._len++] = byte;
  }

  private _extend(data: Uint8Array): void {
    this._ensureCapacity(data.length);
    this.buf.set(data, this._len);
    this._len += data.length;
  }

  private _extendZeros(count: number): void {
    this._ensureCapacity(count);
    this.buf.fill(0, this._len, this._len + count);
    this._len += count;
  }

  private _truncate(len: number): void {
    if (len < this._len) {
      this._len = len;
    }
  }

  // ---- Put methods (32-byte padded chunks) ----

  appendBytes32(b: Uint8Array): void {
    this._extend(b);
    const rest = b.length % 32;
    if (rest !== 0) {
      this._extendZeros(32 - rest);
    }
  }

  putU64(v: bigint): void {
    const tmp = this._tmp;
    // Little-endian 8 bytes
    tmp[0] = Number(v & 0xffn);
    tmp[1] = Number((v >> 8n) & 0xffn);
    tmp[2] = Number((v >> 16n) & 0xffn);
    tmp[3] = Number((v >> 24n) & 0xffn);
    tmp[4] = Number((v >> 32n) & 0xffn);
    tmp[5] = Number((v >> 40n) & 0xffn);
    tmp[6] = Number((v >> 48n) & 0xffn);
    tmp[7] = Number((v >> 56n) & 0xffn);
    tmp.fill(0, 8, 32);
    this._ensureCapacity(32);
    this.buf.set(tmp.subarray(0, 32), this._len);
    this._len += 32;
  }

  putU32(v: number): void {
    const tmp = this._tmp;
    tmp[0] = v & 0xff;
    tmp[1] = (v >> 8) & 0xff;
    tmp[2] = (v >> 16) & 0xff;
    tmp[3] = (v >> 24) & 0xff;
    tmp.fill(0, 4, 32);
    this._ensureCapacity(32);
    this.buf.set(tmp.subarray(0, 32), this._len);
    this._len += 32;
  }

  putU16(v: number): void {
    const tmp = this._tmp;
    tmp[0] = v & 0xff;
    tmp[1] = (v >> 8) & 0xff;
    tmp.fill(0, 2, 32);
    this._ensureCapacity(32);
    this.buf.set(tmp.subarray(0, 32), this._len);
    this._len += 32;
  }

  putU8(v: number): void {
    const tmp = this._tmp;
    tmp[0] = v & 0xff;
    tmp.fill(0, 1, 32);
    this._ensureCapacity(32);
    this.buf.set(tmp.subarray(0, 32), this._len);
    this._len += 32;
  }

  putBool(v: boolean): void {
    const tmp = this._tmp;
    tmp.fill(0, 0, 32);
    if (v) {
      tmp[0] = 1;
    }
    this._ensureCapacity(32);
    this.buf.set(tmp.subarray(0, 32), this._len);
    this._len += 32;
  }

  putBytes(b: Uint8Array): void {
    if (b.length <= 32) {
      this.appendBytes32(b);
      return;
    }
    const idx = this.index();
    this.appendBytes32(b);
    this.merkleize(idx);
  }

  putBitlist(bb: Uint8Array, maxSize: number): void {
    const { data, size } = parseBitlist(bb);
    const idx = this.index();
    this.appendBytes32(data);
    this.merkleizeWithMixin(idx, size, ((maxSize + 255) / 256) | 0);
  }

  putZeroHash(): void {
    this._extendZeros(32);
  }

  // ---- Append methods (no padding) ----

  appendBool(v: boolean): void {
    this._push(v ? 1 : 0);
  }

  appendU8(v: number): void {
    this._push(v & 0xff);
  }

  appendU16(v: number): void {
    this._ensureCapacity(2);
    this.buf[this._len++] = v & 0xff;
    this.buf[this._len++] = (v >> 8) & 0xff;
  }

  appendU32(v: number): void {
    this._ensureCapacity(4);
    this.buf[this._len++] = v & 0xff;
    this.buf[this._len++] = (v >> 8) & 0xff;
    this.buf[this._len++] = (v >> 16) & 0xff;
    this.buf[this._len++] = (v >> 24) & 0xff;
  }

  appendU64(v: bigint): void {
    this._ensureCapacity(8);
    this.buf[this._len++] = Number(v & 0xffn);
    this.buf[this._len++] = Number((v >> 8n) & 0xffn);
    this.buf[this._len++] = Number((v >> 16n) & 0xffn);
    this.buf[this._len++] = Number((v >> 24n) & 0xffn);
    this.buf[this._len++] = Number((v >> 32n) & 0xffn);
    this.buf[this._len++] = Number((v >> 40n) & 0xffn);
    this.buf[this._len++] = Number((v >> 48n) & 0xffn);
    this.buf[this._len++] = Number((v >> 56n) & 0xffn);
  }

  fillUpTo32(): void {
    const rest = this._len % 32;
    if (rest !== 0) {
      this._extendZeros(32 - rest);
    }
  }

  // ---- Merkleization ----

  merkleize(idx: number): void {
    this._merkleizeInner(idx, 0);
  }

  merkleizeWithMixin(idx: number, num: number, limit: number): void {
    this.fillUpTo32();
    this._merkleizeInner(idx, limit);

    // Mix in length: hash(root || encode(num))
    const tmp = this._tmp;
    tmp.set(this.buf.subarray(idx, idx + 32), 0);
    // Little-endian u64 of num
    tmp[32] = num & 0xff;
    tmp[33] = (num >> 8) & 0xff;
    tmp[34] = (num >> 16) & 0xff;
    tmp[35] = (num >> 24) & 0xff;
    // For values > 2^32, we'd need bigint, but num is typically a count
    tmp.fill(0, 36, 64);

    const hash = sha256(tmp);
    this.buf.set(hash, idx);
    // _len should already be idx+32 after merkleizeInner
  }

  merkleizeProgressive(idx: number): void {
    this.fillUpTo32();
    const inputLen = this._len - idx;
    const count = (inputLen / 32) | 0;
    const zh = zeroHashes();

    if (count === 0) {
      this._truncate(idx);
      this._extend(zh[0]);
      return;
    }
    if (count === 1) {
      this._truncate(idx + 32);
      return;
    }

    // EIP-7916 progressive subtree merkleization.
    // Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
    const subtreeSizes: number[] = [];
    {
      let remaining = count;
      const first = Math.min(1, remaining);
      if (first > 0) {
        subtreeSizes.push(first);
        remaining -= first;
      }
      let sz = 4;
      while (remaining > 0) {
        const take = Math.min(sz, remaining);
        subtreeSizes.push(take);
        remaining -= take;
        // Saturating multiply: cap at a safe integer
        sz = sz * 4;
        if (sz > Number.MAX_SAFE_INTEGER) sz = Number.MAX_SAFE_INTEGER;
      }
    }

    // Compute root for each subtree via merkleizeInner
    const subtreeRoots: Uint8Array[] = [];
    let chunkOffset = idx;
    let expectedCap = 1;
    for (const sz of subtreeSizes) {
      const tmpHasher = new Hasher();
      tmpHasher._extend(this.buf.subarray(chunkOffset, chunkOffset + sz * 32));
      tmpHasher._merkleizeInner(0, expectedCap);
      const root = new Uint8Array(32);
      root.set(tmpHasher.buf.subarray(0, 32));
      subtreeRoots.push(root);
      chunkOffset += sz * 32;
      expectedCap = expectedCap * 4;
      if (expectedCap > Number.MAX_SAFE_INTEGER)
        expectedCap = Number.MAX_SAFE_INTEGER;
    }

    // Right-fold: hash(root_0, hash(root_1, hash(root_2, ... hash(root_n-1, zero_hash))))
    let acc = new Uint8Array(zh[0]);
    const tmp = this._tmp;
    for (let i = subtreeRoots.length - 1; i >= 0; i--) {
      tmp.set(subtreeRoots[i], 0);
      tmp.set(acc, 32);
      acc = sha256(tmp);
    }

    this._truncate(idx);
    this._extend(acc);
  }

  merkleizeProgressiveWithMixin(idx: number, num: number): void {
    this.merkleizeProgressive(idx);

    // Mix in length: hash(root || encode(num))
    const tmp = this._tmp;
    tmp.set(this.buf.subarray(idx, idx + 32), 0);
    tmp[32] = num & 0xff;
    tmp[33] = (num >> 8) & 0xff;
    tmp[34] = (num >> 16) & 0xff;
    tmp[35] = (num >> 24) & 0xff;
    tmp.fill(0, 36, 64);

    const hash = sha256(tmp);
    this.buf.set(hash, idx);
  }

  merkleizeProgressiveWithActiveFields(
    idx: number,
    activeFields: Uint8Array
  ): void {
    this.merkleizeProgressive(idx);

    // Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
    const tmp = this._tmp;
    tmp.set(this.buf.subarray(idx, idx + 32), 0);
    tmp.fill(0, 32, 64);
    const copyLen = Math.min(activeFields.length, 32);
    tmp.set(activeFields.subarray(0, copyLen), 32);

    const hash = sha256(tmp);
    this.buf.set(hash, idx);
  }

  private _merkleizeInner(idx: number, limit: number): void {
    const inputLen = this._len - idx;
    const count = ((inputLen + 31) / 32) | 0;
    if (limit === 0) {
      limit = count;
    }
    const zh = zeroHashes();

    if (limit === 0) {
      this._truncate(idx);
      this._extend(zh[0]);
      return;
    }
    if (limit === 1) {
      if (count >= 1 && inputLen >= 32) {
        this._truncate(idx + 32);
      } else {
        this._truncate(idx);
        this._extend(zh[0]);
      }
      return;
    }

    const depth = getDepth(limit);
    if (inputLen === 0) {
      this._truncate(idx);
      this._extend(zh[depth]);
      return;
    }

    // Pad to 32-byte alignment
    const rest = (this._len - idx) % 32;
    if (rest !== 0) {
      this._extendZeros(32 - rest);
    }

    // In-place layer-by-layer hashing
    for (let i = 0; i < depth; i++) {
      const layerLen = (this._len - idx) / 32;

      if (layerLen % 2 === 1) {
        this._extend(zh[i]);
      }

      const pairs = (this._len - idx) / 64;

      // Ensure output buffer is large enough
      const outNeeded = pairs * 32;
      if (this._out.length < outNeeded) {
        this._out = new Uint8Array(outNeeded * 2);
      }

      hashPairs(
        this._out,
        this.buf.subarray(idx, idx + pairs * 64),
        pairs
      );

      this._truncate(idx);
      this._ensureCapacity(outNeeded);
      this.buf.set(this._out.subarray(0, outNeeded), this._len);
      this._len += outNeeded;
    }
  }
}

// ---- HasherPool ----

const _pool: Hasher[] = [];
const MAX_POOL_SIZE = 16;

export class HasherPool {
  static get(): Hasher {
    const h = _pool.pop();
    if (h !== undefined) {
      return h;
    }
    return new Hasher();
  }

  static put(h: Hasher): void {
    h.reset();
    if (_pool.length < MAX_POOL_SIZE) {
      _pool.push(h);
    }
  }
}

// Re-export sha256 for testing purposes
export { sha256 as _sha256 };
