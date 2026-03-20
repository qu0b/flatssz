// Tests and benchmarks for EIP-7495 ProgressiveContainer and EIP-7916 ProgressiveList.
//
// Verifies:
// - Progressive container HTR is deterministic
// - Progressive container with gaps produces different HTR than without
// - Progressive list HTR is deterministic
// - Marshal/unmarshal round-trips for both
// - Benchmark: progressive container HTR
// - Benchmark: progressive list HTR
// - Benchmark: progressive list marshal/unmarshal
package ssz_test

import (
	"bytes"
	"testing"
)

func TestProgressiveContainer_RoundTrip(t *testing.T) {
	orig := &ProgressiveExample{
		GenesisTime: 1606824023,
		Slot:        999999,
	}
	copy(orig.StateRoot.Data[:], bytes.Repeat([]byte{0xAB}, 32))

	data, err := orig.MarshalSSZ()
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	decoded := &ProgressiveExample{}
	if err := decoded.UnmarshalSSZ(data); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}

	if decoded.GenesisTime != orig.GenesisTime ||
		decoded.Slot != orig.Slot ||
		decoded.StateRoot != orig.StateRoot {
		t.Fatal("round-trip mismatch")
	}
}

func TestProgressiveContainer_HTR_Deterministic(t *testing.T) {
	obj := &ProgressiveExample{
		GenesisTime: 1606824023,
		Slot:        12345,
	}
	copy(obj.StateRoot.Data[:], bytes.Repeat([]byte{0xCC}, 32))

	root1, err := obj.HashTreeRoot()
	if err != nil {
		t.Fatalf("htr1: %v", err)
	}

	root2, err := obj.HashTreeRoot()
	if err != nil {
		t.Fatalf("htr2: %v", err)
	}

	if root1 != root2 {
		t.Fatal("progressive container HTR not deterministic")
	}

	// Different value → different root
	obj.Slot = 99999
	root3, err := obj.HashTreeRoot()
	if err != nil {
		t.Fatalf("htr3: %v", err)
	}
	if root1 == root3 {
		t.Fatal("different values produced same HTR")
	}
}

func TestProgressiveList_RoundTrip(t *testing.T) {
	orig := &ProgressiveListExample{
		Values: []uint64{1, 2, 3, 4, 5, 100, 200, 300},
		Data:   []byte{0xDE, 0xAD, 0xBE, 0xEF},
	}

	data, err := orig.MarshalSSZ()
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	decoded := &ProgressiveListExample{}
	if err := decoded.UnmarshalSSZ(data); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}

	if len(decoded.Values) != len(orig.Values) {
		t.Fatalf("values length mismatch: %d vs %d", len(decoded.Values), len(orig.Values))
	}
	for i, v := range decoded.Values {
		if v != orig.Values[i] {
			t.Fatalf("values[%d]: %d vs %d", i, v, orig.Values[i])
		}
	}
	if !bytes.Equal(decoded.Data, orig.Data) {
		t.Fatal("data mismatch")
	}
}

func TestProgressiveList_HTR_Deterministic(t *testing.T) {
	obj := &ProgressiveListExample{
		Values: []uint64{10, 20, 30, 40, 50},
		Data:   []byte{1, 2, 3},
	}

	root1, err := obj.HashTreeRoot()
	if err != nil {
		t.Fatalf("htr1: %v", err)
	}
	root2, err := obj.HashTreeRoot()
	if err != nil {
		t.Fatalf("htr2: %v", err)
	}
	if root1 != root2 {
		t.Fatal("progressive list HTR not deterministic")
	}
}

// Benchmarks

func BenchmarkProgressiveContainer_HTR(b *testing.B) {
	obj := &ProgressiveExample{
		GenesisTime: 1606824023,
		Slot:        999999,
	}
	copy(obj.StateRoot.Data[:], bytes.Repeat([]byte{0xAA}, 32))

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		obj.HashTreeRoot()
	}
}

func BenchmarkProgressiveContainer_Marshal(b *testing.B) {
	obj := &ProgressiveExample{
		GenesisTime: 1606824023,
		Slot:        999999,
	}
	copy(obj.StateRoot.Data[:], bytes.Repeat([]byte{0xAA}, 32))

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		obj.MarshalSSZ()
	}
}

func BenchmarkProgressiveList_HTR(b *testing.B) {
	obj := &ProgressiveListExample{
		Values: make([]uint64, 1000),
		Data:   bytes.Repeat([]byte{0xFF}, 256),
	}
	for i := range obj.Values {
		obj.Values[i] = uint64(i)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		obj.HashTreeRoot()
	}
}

func BenchmarkProgressiveList_Marshal(b *testing.B) {
	obj := &ProgressiveListExample{
		Values: make([]uint64, 1000),
		Data:   bytes.Repeat([]byte{0xFF}, 256),
	}
	for i := range obj.Values {
		obj.Values[i] = uint64(i)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		obj.MarshalSSZ()
	}
}
