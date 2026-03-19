package ssz

// HashWalker defines the interface for SSZ merkle tree computation.
// It is compatible with the dynamic-ssz HashWalker interface.
type HashWalker interface {
	// Hash returns the latest 32-byte hash from the buffer.
	Hash() []byte

	// Append methods add values to the buffer WITHOUT 32-byte padding.
	AppendBool(b bool)
	AppendUint8(i uint8)
	AppendUint16(i uint16)
	AppendUint32(i uint32)
	AppendUint64(i uint64)
	AppendBytes32(b []byte)

	// Put methods add values WITH 32-byte padding (one chunk per value).
	PutUint64(i uint64)
	PutUint32(i uint32)
	PutUint16(i uint16)
	PutUint8(i uint8)
	PutBitlist(bb []byte, maxSize uint64)
	PutBool(b bool)
	PutBytes(b []byte)

	// FillUpTo32 pads the buffer to the next 32-byte boundary.
	FillUpTo32()

	// Append appends raw bytes without padding.
	Append(i []byte)

	// Index returns the current buffer length, used as a merkleization marker.
	Index() int

	// Merkleize hashes the buffer contents from indx to the end into a
	// single 32-byte root, replacing that range in the buffer.
	Merkleize(indx int)

	// MerkleizeWithMixin is like Merkleize but also mixes in a length value,
	// as required by SSZ lists.
	MerkleizeWithMixin(indx int, num, limit uint64)

	// HashRoot returns the final 32-byte hash root from the buffer.
	HashRoot() ([32]byte, error)
}
