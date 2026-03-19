// Package ssz provides runtime support for SSZ (Simple Serialize) encoding,
// decoding, and merkle tree hashing as used by Ethereum's consensus layer.
package ssz

import "errors"

var (
	// ErrBufferTooSmall is returned when the input buffer does not contain
	// enough data for the expected SSZ type.
	ErrBufferTooSmall = errors.New("ssz: buffer too small")

	// ErrInvalidOffset is returned when an offset field in the SSZ encoding
	// is out of range, non-monotonic, or does not match the static section size.
	ErrInvalidOffset = errors.New("ssz: invalid offset")

	// ErrInvalidBool is returned when a boolean byte is not 0x00 or 0x01.
	ErrInvalidBool = errors.New("ssz: invalid bool value")

	// ErrBitlistNoTermination is returned when a bitlist is missing its
	// mandatory termination bit.
	ErrBitlistNoTermination = errors.New("ssz: bitlist missing termination bit")

	// ErrListTooBig is returned when a list's element count exceeds its
	// declared ssz_max limit.
	ErrListTooBig = errors.New("ssz: list exceeds maximum length")
)
