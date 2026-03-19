package ssz

import "crypto/sha256"

// zeroHashes contains precomputed SHA-256 zero hashes for 65 merkle tree
// depth levels. zeroHashes[0] is 32 zero bytes, zeroHashes[i] =
// SHA-256(zeroHashes[i-1] || zeroHashes[i-1]).
var zeroHashes [65][32]byte

func init() {
	var tmp [64]byte
	for i := 0; i < 64; i++ {
		copy(tmp[:32], zeroHashes[i][:])
		copy(tmp[32:], zeroHashes[i][:])
		zeroHashes[i+1] = sha256.Sum256(tmp[:])
	}
}
