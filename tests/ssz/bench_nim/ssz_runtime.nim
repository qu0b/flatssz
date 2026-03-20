## SSZ runtime support for flatssz-generated Nim code.
##
## Provides `Hasher` for merkleization, `HasherPool` (via `getHasher`/`putHasher`)
## for reuse, and `SszError` for decode errors.
##
## Pure Nim — no external dependencies. SHA-256 is implemented inline.

import std/locks

# ---- SHA-256 (pure Nim, FIPS 180-4) ----

const K256: array[64, uint32] = [
  0x428a2f98'u32, 0x71374491'u32, 0xb5c0fbcf'u32, 0xe9b5dba5'u32,
  0x3956c25b'u32, 0x59f111f1'u32, 0x923f82a4'u32, 0xab1c5ed5'u32,
  0xd807aa98'u32, 0x12835b01'u32, 0x243185be'u32, 0x550c7dc3'u32,
  0x72be5d74'u32, 0x80deb1fe'u32, 0x9bdc06a7'u32, 0xc19bf174'u32,
  0xe49b69c1'u32, 0xefbe4786'u32, 0x0fc19dc6'u32, 0x240ca1cc'u32,
  0x2de92c6f'u32, 0x4a7484aa'u32, 0x5cb0a9dc'u32, 0x76f988da'u32,
  0x983e5152'u32, 0xa831c66d'u32, 0xb00327c8'u32, 0xbf597fc7'u32,
  0xc6e00bf3'u32, 0xd5a79147'u32, 0x06ca6351'u32, 0x14292967'u32,
  0x27b70a85'u32, 0x2e1b2138'u32, 0x4d2c6dfc'u32, 0x53380d13'u32,
  0x650a7354'u32, 0x766a0abb'u32, 0x81c2c92e'u32, 0x92722c85'u32,
  0xa2bfe8a1'u32, 0xa81a664b'u32, 0xc24b8b70'u32, 0xc76c51a3'u32,
  0xd192e819'u32, 0xd6990624'u32, 0xf40e3585'u32, 0x106aa070'u32,
  0x19a4c116'u32, 0x1e376c08'u32, 0x2748774c'u32, 0x34b0bcb5'u32,
  0x391c0cb3'u32, 0x4ed8aa4a'u32, 0x5b9cca4f'u32, 0x682e6ff3'u32,
  0x748f82ee'u32, 0x78a5636f'u32, 0x84c87814'u32, 0x8cc70208'u32,
  0x90befffa'u32, 0xa4506ceb'u32, 0xbef9a3f7'u32, 0xc67178f2'u32,
]

proc rotr(x: uint32, n: int): uint32 {.inline.} =
  (x shr n) or (x shl (32 - n))

proc sha256Compress(state: var array[8, uint32], blk: ptr byte) =
  var w: array[64, uint32]
  for i in 0 ..< 16:
    w[i] = (cast[ptr UncheckedArray[byte]](blk)[i*4+0].uint32 shl 24) or
            (cast[ptr UncheckedArray[byte]](blk)[i*4+1].uint32 shl 16) or
            (cast[ptr UncheckedArray[byte]](blk)[i*4+2].uint32 shl 8) or
            (cast[ptr UncheckedArray[byte]](blk)[i*4+3].uint32)
  for i in 16 ..< 64:
    let s0 = rotr(w[i-15], 7) xor rotr(w[i-15], 18) xor (w[i-15] shr 3)
    let s1 = rotr(w[i-2], 17) xor rotr(w[i-2], 19) xor (w[i-2] shr 10)
    w[i] = w[i-16] + s0 + w[i-7] + s1

  var a = state[0]; var b = state[1]; var c = state[2]; var d = state[3]
  var e = state[4]; var f = state[5]; var g = state[6]; var hh = state[7]

  for i in 0 ..< 64:
    let S1 = rotr(e, 6) xor rotr(e, 11) xor rotr(e, 25)
    let ch = (e and f) xor ((not e) and g)
    let temp1 = hh + S1 + ch + K256[i] + w[i]
    let S0 = rotr(a, 2) xor rotr(a, 13) xor rotr(a, 22)
    let maj = (a and b) xor (a and c) xor (b and c)
    let temp2 = S0 + maj
    hh = g; g = f; f = e; e = d + temp1
    d = c; c = b; b = a; a = temp1 + temp2

  state[0] += a; state[1] += b; state[2] += c; state[3] += d
  state[4] += e; state[5] += f; state[6] += g; state[7] += hh

proc sha256*(data: openArray[byte]): array[32, byte] =
  var state: array[8, uint32] = [
    0x6a09e667'u32, 0xbb67ae85'u32, 0x3c6ef372'u32, 0xa54ff53a'u32,
    0x510e527f'u32, 0x9b05688c'u32, 0x1f83d9ab'u32, 0x5be0cd19'u32,
  ]

  let dataLen = data.len
  # Process full 64-byte blocks
  var offset = 0
  while offset + 64 <= dataLen:
    sha256Compress(state, unsafeAddr data[offset])
    offset += 64

  # Final block with padding
  var pad: array[128, byte] # max 2 blocks for final padding
  let remaining = dataLen - offset
  if remaining > 0:
    copyMem(addr pad[0], unsafeAddr data[offset], remaining)
  pad[remaining] = 0x80

  let bitLen = dataLen.uint64 * 8
  var totalPad: int
  if remaining < 56:
    totalPad = 64
  else:
    totalPad = 128

  # Write length in big-endian at the end
  pad[totalPad - 8] = byte(bitLen shr 56)
  pad[totalPad - 7] = byte(bitLen shr 48)
  pad[totalPad - 6] = byte(bitLen shr 40)
  pad[totalPad - 5] = byte(bitLen shr 32)
  pad[totalPad - 4] = byte(bitLen shr 24)
  pad[totalPad - 3] = byte(bitLen shr 16)
  pad[totalPad - 2] = byte(bitLen shr 8)
  pad[totalPad - 1] = byte(bitLen)

  var pOff = 0
  while pOff < totalPad:
    sha256Compress(state, addr pad[pOff])
    pOff += 64

  # Write output in big-endian
  for i in 0 ..< 8:
    result[i*4+0] = byte(state[i] shr 24)
    result[i*4+1] = byte(state[i] shr 16)
    result[i*4+2] = byte(state[i] shr 8)
    result[i*4+3] = byte(state[i])

proc sha256Pair(a, b: openArray[byte]): array[32, byte] =
  ## Hash the concatenation of two 32-byte inputs.
  var combined: array[64, byte]
  copyMem(addr combined[0], unsafeAddr a[0], 32)
  copyMem(addr combined[32], unsafeAddr b[0], 32)
  sha256(combined)

# ---- Errors ----

type SszError* = object of CatchableError

proc newSszErrorBufferTooSmall*(): ref SszError =
  newException(SszError, "ssz: buffer too small")

proc newSszErrorInvalidOffset*(): ref SszError =
  newException(SszError, "ssz: invalid offset")

proc newSszErrorInvalidBool*(): ref SszError =
  newException(SszError, "ssz: invalid bool value")

# ---- Zero hashes ----

var zeroHashes*: array[65, array[32, byte]]

proc computeZeroHashes() =
  for i in 0 ..< 64:
    zeroHashes[i + 1] = sha256Pair(zeroHashes[i], zeroHashes[i])

computeZeroHashes()

# ---- Utility ----

proc getDepth(d: uint64): uint8 =
  if d <= 1:
    return 0
  var v = d
  # next power of two
  v -= 1
  v = v or (v shr 1)
  v = v or (v shr 2)
  v = v or (v shr 4)
  v = v or (v shr 8)
  v = v or (v shr 16)
  v = v or (v shr 32)
  v += 1
  # count trailing zeros = log2 of power of two
  var bits: uint8 = 0
  var tmp = v
  while tmp > 1:
    tmp = tmp shr 1
    bits += 1
  return bits

proc parseBitlist(buf: openArray[byte]): (seq[byte], uint64) =
  if buf.len == 0:
    return (@[], 0'u64)
  let last = buf[buf.len - 1]
  if last == 0:
    return (@[], 0'u64)
  var msb: uint8 = 0
  var tmp = last
  while tmp > 1:
    tmp = tmp shr 1
    msb += 1
  let size = 8'u64 * (buf.len.uint64 - 1) + msb.uint64
  var dst = newSeq[byte](buf.len)
  copyMem(addr dst[0], unsafeAddr buf[0], buf.len)
  dst[dst.len - 1] = dst[dst.len - 1] and (not (1'u8 shl msb))
  while dst.len > 0 and dst[dst.len - 1] == 0:
    dst.setLen(dst.len - 1)
  return (dst, size)

# ---- Hasher ----

type Hasher* = object
  buf*: seq[byte]
  tmp*: array[64, byte]

proc newHasher*(): Hasher =
  result.buf = newSeqOfCap[byte](8192)

proc reset*(h: var Hasher) =
  h.buf.setLen(0)

proc index*(h: Hasher): int =
  h.buf.len

proc finish*(h: Hasher): array[32, byte] =
  if h.buf.len >= 32:
    copyMem(addr result[0], unsafeAddr h.buf[h.buf.len - 32], 32)

proc hashRoot*(h: Hasher): array[32, byte] =
  ## Alias for finish.
  h.finish()

# ---- Put methods (32-byte padded chunks) ----

proc appendBytes32*(h: var Hasher, b: openArray[byte]) =
  if b.len > 0:
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + b.len)
    copyMem(addr h.buf[oldLen], unsafeAddr b[0], b.len)
  let rest = b.len mod 32
  if rest != 0:
    let pad = 32 - rest
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + pad)
    zeroMem(addr h.buf[oldLen], pad)

proc putUint64*(h: var Hasher, v: uint64) =
  let le = v  # Nim is little-endian on x86
  copyMem(addr h.tmp[0], unsafeAddr le, 8)
  zeroMem(addr h.tmp[8], 24)
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr h.tmp[0], 32)

proc putUint32*(h: var Hasher, v: uint32) =
  let le = v
  copyMem(addr h.tmp[0], unsafeAddr le, 4)
  zeroMem(addr h.tmp[4], 28)
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr h.tmp[0], 32)

proc putUint16*(h: var Hasher, v: uint16) =
  let le = v
  copyMem(addr h.tmp[0], unsafeAddr le, 2)
  zeroMem(addr h.tmp[2], 30)
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr h.tmp[0], 32)

proc putUint8*(h: var Hasher, v: uint8) =
  h.tmp[0] = v
  zeroMem(addr h.tmp[1], 31)
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr h.tmp[0], 32)

proc putBool*(h: var Hasher, v: bool) =
  zeroMem(addr h.tmp[0], 32)
  if v:
    h.tmp[0] = 1
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr h.tmp[0], 32)

proc putZeroHash*(h: var Hasher) =
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  zeroMem(addr h.buf[oldLen], 32)

# ---- Append methods (no padding) ----

proc appendBool*(h: var Hasher, v: bool) =
  h.buf.add(if v: 1'u8 else: 0'u8)

proc appendUint8*(h: var Hasher, v: uint8) =
  h.buf.add(v)

proc appendUint16*(h: var Hasher, v: uint16) =
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 2)
  let le = v
  copyMem(addr h.buf[oldLen], unsafeAddr le, 2)

proc appendUint32*(h: var Hasher, v: uint32) =
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 4)
  let le = v
  copyMem(addr h.buf[oldLen], unsafeAddr le, 4)

proc appendUint64*(h: var Hasher, v: uint64) =
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 8)
  let le = v
  copyMem(addr h.buf[oldLen], unsafeAddr le, 8)

proc fillUpTo32*(h: var Hasher) =
  let rest = h.buf.len mod 32
  if rest != 0:
    let pad = 32 - rest
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + pad)
    zeroMem(addr h.buf[oldLen], pad)

# ---- Batch hashing helper ----

proc hashPairs(output: var seq[byte], input: openArray[byte], pairs: int) =
  ## Hash `pairs` consecutive 64-byte pair blocks from `input` into `output`.
  ## Each pair of 32-byte chunks produces one 32-byte hash.
  output.setLen(pairs * 32)
  for i in 0 ..< pairs:
    let h = sha256(input[i * 64 ..< i * 64 + 64])
    copyMem(addr output[i * 32], unsafeAddr h[0], 32)

# ---- Merkleization ----

proc merkleizeInner(h: var Hasher, idx: int, limitIn: uint64) =
  let inputLen = h.buf.len - idx
  let count = ((inputLen + 31) div 32).uint64
  var limit = limitIn
  if limit == 0:
    limit = count

  if limit == 0:
    h.buf.setLen(idx)
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + 32)
    copyMem(addr h.buf[oldLen], addr zeroHashes[0][0], 32)
    return

  if limit == 1:
    if count >= 1 and inputLen >= 32:
      h.buf.setLen(idx + 32)
    else:
      h.buf.setLen(idx)
      let oldLen = h.buf.len
      h.buf.setLen(oldLen + 32)
      copyMem(addr h.buf[oldLen], addr zeroHashes[0][0], 32)
    return

  let depth = getDepth(limit)
  if inputLen == 0:
    h.buf.setLen(idx)
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + 32)
    copyMem(addr h.buf[oldLen], addr zeroHashes[depth.int][0], 32)
    return

  # Pad to 32-byte alignment
  let rest = (h.buf.len - idx) mod 32
  if rest != 0:
    let pad = 32 - rest
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + pad)
    zeroMem(addr h.buf[oldLen], pad)

  # In-place layer-by-layer hashing
  var outBuf: seq[byte] = @[]
  for i in 0'u8 ..< depth:
    let layerLen = (h.buf.len - idx) div 32
    if layerLen mod 2 == 1:
      let oldLen = h.buf.len
      h.buf.setLen(oldLen + 32)
      copyMem(addr h.buf[oldLen], addr zeroHashes[i.int][0], 32)

    let pairs = (h.buf.len - idx) div 64
    hashPairs(outBuf, h.buf.toOpenArray(idx, idx + pairs * 64 - 1), pairs)
    h.buf.setLen(idx)
    if outBuf.len > 0:
      let oldLen = h.buf.len
      h.buf.setLen(oldLen + outBuf.len)
      copyMem(addr h.buf[oldLen], addr outBuf[0], outBuf.len)

proc merkleize*(h: var Hasher, idx: int) =
  h.merkleizeInner(idx, 0)

proc merkleizeWithMixin*(h: var Hasher, idx: int, num: uint64, limit: uint64) =
  h.fillUpTo32()
  h.merkleizeInner(idx, limit)

  # Mix in length: hash(root || encode(num))
  assert h.buf.len == idx + 32
  copyMem(addr h.tmp[0], addr h.buf[idx], 32)
  let le = num
  copyMem(addr h.tmp[32], unsafeAddr le, 8)
  zeroMem(addr h.tmp[40], 24)

  let hash = sha256(h.tmp)
  copyMem(addr h.buf[idx], unsafeAddr hash[0], 32)

proc putBytes*(h: var Hasher, b: openArray[byte]) =
  if b.len <= 32:
    h.appendBytes32(b)
    return
  let idx = h.index()
  h.appendBytes32(b)
  h.merkleize(idx)

proc putBitlist*(h: var Hasher, bb: openArray[byte], maxSize: uint64) =
  let (bitlist, size) = parseBitlist(bb)
  let idx = h.index()
  h.appendBytes32(bitlist)
  h.merkleizeWithMixin(idx, size, (maxSize + 255) div 256)

proc merkleizeProgressive*(h: var Hasher, idx: int) =
  h.fillUpTo32()
  let inputLen = h.buf.len - idx
  let count = inputLen div 32

  if count == 0:
    h.buf.setLen(idx)
    let oldLen = h.buf.len
    h.buf.setLen(oldLen + 32)
    copyMem(addr h.buf[oldLen], addr zeroHashes[0][0], 32)
    return

  if count == 1:
    h.buf.setLen(idx + 32)
    return

  # EIP-7916 progressive subtree merkleization.
  # Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
  var subtreeSizes: seq[int] = @[]
  block:
    var remaining = count
    let first = min(1, remaining)
    if first > 0:
      subtreeSizes.add(first)
      remaining -= first
    var sz = 4
    while remaining > 0:
      let take = min(sz, remaining)
      subtreeSizes.add(take)
      remaining -= take
      # saturating multiply
      if sz <= high(int) div 4:
        sz *= 4
      else:
        sz = high(int)

  # Compute root for each subtree via merkleizeInner
  var subtreeRoots: seq[array[32, byte]] = @[]
  var chunkOffset = idx
  var expectedCap = 1
  for sz in subtreeSizes:
    var tmpHasher = newHasher()
    let srcLen = sz * 32
    tmpHasher.buf.setLen(srcLen)
    copyMem(addr tmpHasher.buf[0], addr h.buf[chunkOffset], srcLen)
    tmpHasher.merkleizeInner(0, expectedCap.uint64)
    var root: array[32, byte]
    copyMem(addr root[0], addr tmpHasher.buf[0], 32)
    subtreeRoots.add(root)
    chunkOffset += srcLen
    if expectedCap <= high(int) div 4:
      expectedCap *= 4
    else:
      expectedCap = high(int)

  # Right-fold: hash(root_0, hash(root_1, hash(root_2, ... hash(root_n-1, zero_hash))))
  var acc = zeroHashes[0]
  for i in countdown(subtreeRoots.len - 1, 0):
    copyMem(addr h.tmp[0], addr subtreeRoots[i][0], 32)
    copyMem(addr h.tmp[32], addr acc[0], 32)
    acc = sha256(h.tmp)

  h.buf.setLen(idx)
  let oldLen = h.buf.len
  h.buf.setLen(oldLen + 32)
  copyMem(addr h.buf[oldLen], addr acc[0], 32)

proc merkleizeProgressiveWithMixin*(h: var Hasher, idx: int, num: uint64) =
  h.merkleizeProgressive(idx)

  # Mix in length: hash(root || encode(num))
  assert h.buf.len == idx + 32
  copyMem(addr h.tmp[0], addr h.buf[idx], 32)
  let le = num
  copyMem(addr h.tmp[32], unsafeAddr le, 8)
  zeroMem(addr h.tmp[40], 24)
  let hash = sha256(h.tmp)
  copyMem(addr h.buf[idx], unsafeAddr hash[0], 32)

proc merkleizeProgressiveWithActiveFields*(h: var Hasher, idx: int,
    activeFields: openArray[byte]) =
  h.merkleizeProgressive(idx)

  # Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
  assert h.buf.len == idx + 32
  copyMem(addr h.tmp[0], addr h.buf[idx], 32)
  zeroMem(addr h.tmp[32], 32)
  let copyLen = min(activeFields.len, 32)
  if copyLen > 0:
    copyMem(addr h.tmp[32], unsafeAddr activeFields[0], copyLen)
  let hash = sha256(h.tmp)
  copyMem(addr h.buf[idx], unsafeAddr hash[0], 32)

# ---- HasherPool ----

var poolLock: Lock
var pool: seq[Hasher]
const poolMaxSize = 16

initLock(poolLock)

proc getHasher*(): Hasher =
  acquire(poolLock)
  if pool.len > 0:
    result = pool.pop()
    release(poolLock)
    result.reset()
  else:
    release(poolLock)
    result = newHasher()

proc putHasher*(h: var Hasher) =
  h.reset()
  acquire(poolLock)
  if pool.len < poolMaxSize:
    pool.add(move h)
  release(poolLock)

# ---- HasherPool object (codegen compatibility) ----

type HasherPool* = object

proc get*(T: typedesc[HasherPool]): Hasher =
  getHasher()

proc put*(T: typedesc[HasherPool], h: var Hasher) =
  putHasher(h)
