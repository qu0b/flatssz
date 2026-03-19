# flatssz

Cross-language SSZ (Simple Serialize) code generation from [FlatBuffers](https://github.com/google/flatbuffers) schemas. Define Ethereum consensus layer types once in `.fbs` and generate Go and Rust code implementing marshal, unmarshal, and hash tree root.

## Supported Languages

| Language | Flag | Runtime |
|---|---|---|
| **Go** | `--ssz-go` | [`dynamic-ssz/hasher`](https://github.com/pk910/dynamic-ssz) (SIMD SHA-256) + `go/ssz` (errors) |
| **Rust** | `--ssz-rust` | `ssz_flatbuffers` crate (`sha2` with SHA-NI) |

## Quick Start

### 1. Build flatc

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
make -j
```

### 2. Define your schema

```fbs
namespace eth;

struct Checkpoint {
  epoch:ulong;
  root:[ubyte:32];
}

struct BeaconBlockHeader {
  slot:ulong;
  proposer_index:ulong;
  parent_root:[ubyte:32];
  state_root:[ubyte:32];
  body_root:[ubyte:32];
}

table AttestationData {
  slot:ulong;
  index:ulong;
  beacon_block_root:Bytes32;
  source:Checkpoint;
  target:Checkpoint;
}
```

### 3. Generate code

```bash
# Go
./flatc --ssz-go -o output/ schema.fbs

# Rust
./flatc --ssz-rust -o output/ schema.fbs
```

### 4. Use the generated types

**Go:**
```go
header := &BeaconBlockHeader{Slot: 12345, ProposerIndex: 42}

data, err := header.MarshalSSZ()
decoded := &BeaconBlockHeader{}
err = decoded.UnmarshalSSZ(data)
root, err := header.HashTreeRoot()
```

**Rust:**
```rust
let header = BeaconBlockHeader { slot: 12345, proposer_index: 42, ..Default::default() };

let data = header.as_ssz_bytes();
let decoded = BeaconBlockHeader::from_ssz_bytes(&data)?;
let root = header.tree_hash_root();
```

## SSZ Attributes

| Attribute | Purpose | Example |
|---|---|---|
| `ssz_max` | List capacity limit (required for `[T]` vectors and `string`) | `(ssz_max:"2048")` |
| `ssz_size` | Fixed byte size for byte vectors | `(ssz_size:"32")` |
| `ssz_bitsize` | Bit-level size for bitvectors | `(ssz_bitsize:"64")` |
| `ssz_bitlist` | Marks `[ubyte]` as SSZ bitlist type | `(ssz_bitlist)` |
| `ssz_bitvector` | Marks `[ubyte:N]` as SSZ bitvector type | `(ssz_bitvector)` |

Comma-separated `ssz_max` for nested lists: `(ssz_max:"1048576,1073741824")` sets outer and inner limits.

## Type Mapping

| FlatBuffers | SSZ Type | Go | Rust |
|---|---|---|---|
| `bool` | Bool | `bool` | `bool` |
| `ubyte` | Uint8 | `uint8` | `u8` |
| `ushort` | Uint16 | `uint16` | `u16` |
| `uint` | Uint32 | `uint32` | `u32` |
| `ulong` | Uint64 | `uint64` | `u64` |
| `[ubyte:32]` | Vector[Uint8, 32] | `[32]byte` | `[u8; 32]` |
| `[T]` + `ssz_max` | List[T] | `[]T` | `Vec<T>` |
| `[T:N]` | Vector[T, N] | `[N]T` | `[T; N]` |
| `[ubyte]` + `ssz_bitlist` | Bitlist | `[]byte` | `Vec<u8>` |
| `[string]` + `ssz_max` | List[List[byte]] | `[][]byte` | `Vec<Vec<u8>>` |
| `struct` (all fixed) | Container (fixed) | value struct | value struct |
| `table` | Container (dynamic) | value struct | value struct |

## Generated Methods

**Go:**
- `SizeSSZ() int`
- `MarshalSSZ() ([]byte, error)` / `MarshalSSZTo(buf []byte) ([]byte, error)`
- `UnmarshalSSZ(buf []byte) error`
- `HashTreeRoot() ([32]byte, error)` / `HashTreeRootWith(hh sszutils.HashWalker) error`

**Rust:**
- `ssz_bytes_len(&self) -> usize`
- `as_ssz_bytes(&self) -> Vec<u8>` / `ssz_append(&self, buf: &mut Vec<u8>)`
- `from_ssz_bytes(bytes: &[u8]) -> Result<Self, SszError>`
- `tree_hash_root(&self) -> [u8; 32]` / `tree_hash_with(&self, h: &mut Hasher)`

## Benchmarks

All benchmarks on a Deneb `SignedBeaconBlock` (~130KB of real Ethereum mainnet data). Verified correct against known hash tree roots.

### Cross-language

| Operation | Go | Rust |
|---|---|---|
| **Unmarshal** | 31.5 us | **12.4 us** (2.5x) |
| **Marshal** | 15.7 us | **3.8 us** (4.1x) |
| **HashTreeRoot** | **409 us** (1.9x) | 775 us |

Same `.fbs` schema, same generated code patterns. Rust wins on marshal/unmarshal (zero-cost abstractions, no GC). Go wins on HTR (batch SIMD SHA-256 via [hashtree](https://github.com/prysmaticlabs/hashtree) cgo).

### Go: flatssz vs other SSZ libraries

| Operation | flatssz | dynamic-ssz codegen | fastssz v2 |
|---|---|---|---|
| **Unmarshal** | **31.5 us** (485 allocs) | 42.2 us (1515 allocs) | 49.9 us (1669 allocs) |
| **Marshal** | 15.7 us (1 alloc) | **14.6 us** (1 alloc) | 15.4 us (1 alloc) |
| **HashTreeRoot** | 409 us (0 allocs) | **402 us** (0 allocs) | 784 us (32 allocs) |

### Why unmarshal is faster

flatssz uses **value types** for fixed-size containers. A `Checkpoint` field is an inline struct, not a heap-allocated pointer. This eliminates allocations at the field level — 485 total allocs vs 1515+ in pointer-based libraries.

### Memory retention

Other SSZ libraries use pointer types (`[]*Validator`) which require a heap allocation per element. A common argument for pointers is that value types retain the parent object in memory. This is incorrect — indexing a value-type slice (`state.Validators[i]`) produces a **copy**, so the parent is free to be GC'd.

Benchmarked with 1M validators (~122MB):

| Metric | Value types (`[]Validator`) | Pointer types (`[]*Validator`) |
|---|---|---|
| Allocation time | **13.7 ms** | 32.3 ms |
| Heap allocations | **1** | 1,000,001 |
| Memory after extracting 1 element + GC | **0 MB retained** | 0 MB retained |

Both approaches correctly free the parent state. Value types are 2.4x faster to allocate with 1M fewer heap objects.

See `tests/ssz/memory_bench/` for the full benchmark.

## Project Structure

```
src/idl_gen_ssz_go.cpp        -- Go SSZ code generator
src/idl_gen_ssz_rust.cpp      -- Rust SSZ code generator
go/ssz/                        -- Go runtime (error types)
rust/ssz_flatbuffers/          -- Rust runtime crate (Hasher, HasherPool, SszError)
tests/ssz/                     -- Test schemas and benchmarks
```

## Limitations

- `uint16` array length limit (65535) prevents some BeaconState fields (e.g. `EPOCHS_PER_HISTORICAL_VECTOR = 65536`)
- Fixed-length arrays (`[T:N]`) cannot appear directly in tables — wrap in a struct
- No schema evolution (SSZ requires all fields present)

## Upstream

Fork of [google/flatbuffers](https://github.com/google/flatbuffers) with SSZ code generation backends added. All existing FlatBuffers functionality is preserved.

## License

Apache License, Version 2.0. See [LICENSE](LICENSE).
