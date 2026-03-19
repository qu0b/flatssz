package ssz

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math/bits"
)

// Compile-time interface check.
var _ HashWalker = (*Hasher)(nil)

var (
	trueBytes  [32]byte
	falseBytes [32]byte
	zeroBytes  [32]byte
)

func init() {
	trueBytes[0] = 1
}

// Hasher implements HashWalker using a flat byte buffer of 32-byte chunks.
type Hasher struct {
	buf []byte
	tmp [64]byte
}

// NewHasher creates a new Hasher.
func NewHasher() *Hasher {
	return &Hasher{}
}

// Reset clears the hasher for reuse.
func (h *Hasher) Reset() {
	h.buf = h.buf[:0]
}

// AppendBytes32 appends b, right-padding with zeros to a 32-byte boundary.
func (h *Hasher) AppendBytes32(b []byte) {
	h.buf = append(h.buf, b...)
	if rest := len(b) % 32; rest != 0 {
		h.buf = append(h.buf, zeroBytes[:32-rest]...)
	}
}

// PutUint64 appends a little-endian uint64 in a full 32-byte chunk.
func (h *Hasher) PutUint64(i uint64) {
	binary.LittleEndian.PutUint64(h.tmp[:8], i)
	h.AppendBytes32(h.tmp[:8])
}

// PutUint32 appends a little-endian uint32 in a full 32-byte chunk.
func (h *Hasher) PutUint32(i uint32) {
	binary.LittleEndian.PutUint32(h.tmp[:4], i)
	h.AppendBytes32(h.tmp[:4])
}

// PutUint16 appends a little-endian uint16 in a full 32-byte chunk.
func (h *Hasher) PutUint16(i uint16) {
	binary.LittleEndian.PutUint16(h.tmp[:2], i)
	h.AppendBytes32(h.tmp[:2])
}

// PutUint8 appends a uint8 in a full 32-byte chunk.
func (h *Hasher) PutUint8(i uint8) {
	h.tmp[0] = i
	h.AppendBytes32(h.tmp[:1])
}

// PutBool appends a boolean as a full 32-byte chunk (0x01... or 0x00...).
func (h *Hasher) PutBool(b bool) {
	if b {
		h.buf = append(h.buf, trueBytes[:]...)
	} else {
		h.buf = append(h.buf, falseBytes[:]...)
	}
}

// PutBytes appends bytes. If len(b) <= 32 it is zero-padded to 32 bytes.
// If longer, the bytes are merkleized into a single 32-byte root.
func (h *Hasher) PutBytes(b []byte) {
	if len(b) <= 32 {
		h.AppendBytes32(b)
		return
	}
	indx := h.Index()
	h.AppendBytes32(b)
	h.Merkleize(indx)
}

// PutBitlist appends an SSZ bitlist and merkleizes it with a length mixin.
func (h *Hasher) PutBitlist(bb []byte, maxSize uint64) {
	bitlist, size := parseBitlist(bb)
	indx := h.Index()
	h.AppendBytes32(bitlist)
	h.MerkleizeWithMixin(indx, size, (maxSize+255)/256)
}

// AppendBool appends a single byte (0 or 1) without 32-byte padding.
func (h *Hasher) AppendBool(b bool) {
	if b {
		h.buf = append(h.buf, 1)
	} else {
		h.buf = append(h.buf, 0)
	}
}

// AppendUint8 appends a uint8 without padding.
func (h *Hasher) AppendUint8(i uint8) {
	h.buf = append(h.buf, i)
}

// AppendUint16 appends a little-endian uint16 without padding.
func (h *Hasher) AppendUint16(i uint16) {
	h.buf = binary.LittleEndian.AppendUint16(h.buf, i)
}

// AppendUint32 appends a little-endian uint32 without padding.
func (h *Hasher) AppendUint32(i uint32) {
	h.buf = binary.LittleEndian.AppendUint32(h.buf, i)
}

// AppendUint64 appends a little-endian uint64 without padding.
func (h *Hasher) AppendUint64(i uint64) {
	h.buf = binary.LittleEndian.AppendUint64(h.buf, i)
}

// Append appends raw bytes without padding.
func (h *Hasher) Append(i []byte) {
	h.buf = append(h.buf, i...)
}

// FillUpTo32 pads the buffer with zeros to the next 32-byte boundary.
func (h *Hasher) FillUpTo32() {
	if rest := len(h.buf) % 32; rest != 0 {
		h.buf = append(h.buf, zeroBytes[:32-rest]...)
	}
}

// Index returns the current buffer length as a merkleization starting marker.
func (h *Hasher) Index() int {
	return len(h.buf)
}

// Hash returns the last 32 bytes of the buffer.
func (h *Hasher) Hash() []byte {
	start := 0
	if len(h.buf) > 32 {
		start = len(h.buf) - 32
	}
	return h.buf[start:]
}

// HashRoot returns the final 32-byte hash root.
func (h *Hasher) HashRoot() (res [32]byte, err error) {
	if len(h.buf) != 32 {
		err = fmt.Errorf("ssz: expected 32 byte buffer, got %d", len(h.buf))
		return
	}
	copy(res[:], h.buf)
	return
}

// Merkleize hashes the buffer from indx to end into a single 32-byte root.
func (h *Hasher) Merkleize(indx int) {
	input := h.buf[indx:]
	input = merkleizeImpl(input[:0], input, 0)
	h.buf = append(h.buf[:indx], input...)
}

// MerkleizeWithMixin hashes and mixes in a length value for SSZ lists.
func (h *Hasher) MerkleizeWithMixin(indx int, num, limit uint64) {
	h.FillUpTo32()
	input := h.buf[indx:]

	input = merkleizeImpl(input[:0], input, limit)

	// Mix in length
	binary.LittleEndian.PutUint64(h.tmp[:8], num)
	clear(h.tmp[8:32])
	input = append(input, h.tmp[:32]...)

	// Hash the [root || length] pair
	sum := sha256.Sum256(input)
	h.buf = append(h.buf[:indx], sum[:]...)
}

// getDepth returns the tree depth for limit chunks.
func getDepth(d uint64) uint8 {
	if d <= 1 {
		return 0
	}
	i := nextPowerOfTwo(d)
	return 64 - uint8(bits.LeadingZeros64(i)) - 1
}

// nextPowerOfTwo returns the smallest power of two >= v.
func nextPowerOfTwo(v uint64) uint64 {
	if v == 0 {
		return 1
	}
	v--
	v |= v >> 1
	v |= v >> 2
	v |= v >> 4
	v |= v >> 8
	v |= v >> 16
	v |= v >> 32
	v++
	return v
}

// merkleizeImpl performs the core merkleization loop.
func merkleizeImpl(dst, input []byte, limit uint64) []byte {
	count := uint64((len(input) + 31) / 32)
	if limit == 0 {
		limit = count
	}

	if limit == 0 {
		return append(dst, zeroBytes[:]...)
	}
	if limit == 1 {
		if count == 1 {
			return append(dst, input[:32]...)
		}
		return append(dst, zeroBytes[:]...)
	}

	depth := getDepth(limit)
	if len(input) == 0 {
		return append(dst, zeroHashes[depth][:]...)
	}

	for i := uint8(0); i < depth; i++ {
		layerLen := len(input) / 32
		oddNodeLength := layerLen%2 == 1

		if oddNodeLength {
			input = append(input, zeroHashes[i][:]...)
			layerLen++
		}

		outputLen := (layerLen / 2) * 32
		hashChunks(input, input)
		input = input[:outputLen]
	}

	return append(dst, input...)
}

// hashChunks hashes consecutive 64-byte pairs in-place using SHA-256.
func hashChunks(dst, src []byte) {
	for i := 0; i+64 <= len(src); i += 64 {
		sum := sha256.Sum256(src[i : i+64])
		copy(dst[i/2:], sum[:])
	}
}

// parseBitlist decodes an SSZ-encoded bitlist, returning the raw bit data
// and the logical bit count.
func parseBitlist(buf []byte) ([]byte, uint64) {
	if len(buf) == 0 {
		return nil, 0
	}
	msb := uint8(bits.Len8(buf[len(buf)-1]))
	if msb == 0 {
		return nil, 0
	}
	msb--
	size := uint64(8*(len(buf)-1) + int(msb))

	dst := make([]byte, len(buf))
	copy(dst, buf)
	dst[len(dst)-1] &^= 1 << msb

	// Trim trailing zeros
	newLen := len(dst)
	for i := len(dst) - 1; i >= 0; i-- {
		if dst[i] != 0x00 {
			break
		}
		newLen = i
	}
	return dst[:newLen], size
}
