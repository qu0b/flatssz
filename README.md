# flatssz

SSZ (Simple Serialize) code generation backend for [FlatBuffers](https://github.com/google/flatbuffers). Define Ethereum consensus layer types once in `.fbs` schemas and generate Go code implementing `MarshalSSZ`, `UnmarshalSSZ`, `SizeSSZ`, and `HashTreeRoot`.

## Why

Ethereum's consensus layer uses SSZ for all data serialization and merkleization. Existing SSZ codegen tools are language-specific (Go struct tags, reflection). FlatBuffers provides a mature, cross-language IDL with an extensible code generator plugin system. This project adds an `--ssz-go` backend to `flatc`, enabling `.fbs` as the single schema source for SSZ encoding.

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

### 3. Generate SSZ code

```bash
./flatc --ssz-go -o output/ schema.fbs
```

### 4. Use the generated types

```go
header := &BeaconBlockHeader{
    Slot:          12345,
    ProposerIndex: 42,
}

// Serialize
data, err := header.MarshalSSZ()

// Deserialize
decoded := &BeaconBlockHeader{}
err = decoded.UnmarshalSSZ(data)

// Merkle root
root, err := header.HashTreeRoot()
```

## SSZ Attributes

Annotate `.fbs` fields with SSZ metadata:

| Attribute | Purpose | Example |
|---|---|---|
| `ssz_max` | List capacity limit (required for `[T]` vectors and `string`) | `(ssz_max:"2048")` |
| `ssz_size` | Fixed byte size for byte vectors | `(ssz_size:"32")` |
| `ssz_bitsize` | Bit-level size for bitvectors | `(ssz_bitsize:"64")` |
| `ssz_bitlist` | Marks `[ubyte]` as SSZ bitlist type | `(ssz_bitlist)` |
| `ssz_bitvector` | Marks `[ubyte:N]` as SSZ bitvector type | `(ssz_bitvector)` |

Comma-separated `ssz_max` for nested lists: `(ssz_max:"1048576,1073741824")` sets outer and inner limits.

## Type Mapping

| FlatBuffers | SSZ Type | Go Type |
|---|---|---|
| `bool` | Bool | `bool` |
| `ubyte` / `byte` | Uint8 | `uint8` |
| `ushort` / `short` | Uint16 | `uint16` |
| `uint` / `int` | Uint32 | `uint32` |
| `ulong` / `long` | Uint64 | `uint64` |
| `[ubyte:32]` (struct) | Vector[Uint8, 32] | `[32]byte` |
| `[T]` + `ssz_max` | List[T] | `[]T` |
| `[T:N]` (struct) | Vector[T, N] | `[N]T` |
| `[ubyte]` + `ssz_bitlist` | Bitlist | `[]byte` |
| `[ubyte:N]` + `ssz_bitvector` | Bitvector | `[N]byte` |
| `[string]` + `ssz_max` | List[List[byte]] | `[][]byte` |
| `struct` (all fixed) | Container (fixed) | `struct` |
| `table` | Container (dynamic) | `struct` |

## Generated Methods

For each struct/table, the generator produces:

- `SizeSSZ() int` -- byte size (compile-time constant for fixed types)
- `MarshalSSZ() ([]byte, error)` -- serialize to new buffer
- `MarshalSSZTo(buf []byte) ([]byte, error)` -- serialize appending to existing buffer
- `UnmarshalSSZ(buf []byte) error` -- deserialize from buffer
- `HashTreeRoot() ([32]byte, error)` -- compute SSZ merkle root
- `HashTreeRootWith(hh *ssz.Hasher) error` -- compute root using pooled hasher

## Go Runtime Library

The `go/ssz/` package provides the runtime support:

- `Hasher` -- buffer-based SHA-256 merkleization engine
- `HasherPool` -- `sync.Pool` for `Hasher` reuse
- `HashWalker` -- interface compatible with [dynamic-ssz](https://github.com/pk910/dynamic-ssz)
- Precomputed zero hashes for 65 merkle tree depth levels
- Error types: `ErrBufferTooSmall`, `ErrInvalidOffset`, `ErrInvalidBool`, `ErrBitlistNoTermination`, `ErrListTooBig`

## Benchmarks

Block mainnet benchmarks (Deneb `SignedBeaconBlock`, ~130KB) against established SSZ libraries:

| Operation | flatssz | dynamic-ssz codegen | fastssz v2 |
|---|---|---|---|
| **Unmarshal** | **30.5 us** (485 allocs) | 42.2 us (1515 allocs) | 49.9 us (1669 allocs) |
| **Marshal** | 19.3 us (1 alloc) | **14.6 us** (1 alloc) | 15.4 us (1 alloc) |
| **HashTreeRoot** | 912 us (128 allocs) | **402 us** (0 allocs) | 784 us (32 allocs) |

Unmarshal is fastest due to direct codegen with minimal allocations. Marshal is competitive. HashTreeRoot gap is due to using standard `crypto/sha256` vs SIMD-accelerated hashing in dynamic-ssz.

Benchmark source: [ssz-benchmark](https://github.com/pk910/ssz-benchmark)

## Test Schemas

See `tests/ssz/` for example schemas:

- `basic_types.fbs` -- all scalar types
- `container_fixed.fbs` -- fixed-size container (BeaconBlockHeader pattern)
- `container_dynamic.fbs` -- mixed fixed/dynamic fields
- `vectors_lists.fbs` -- fixed arrays and variable lists
- `bitfields.fbs` -- bitvector and bitlist
- `beacon_types.fbs` -- real Ethereum types (Attestation, etc.)

## Project Structure

```
src/idl_gen_ssz_go.cpp    -- SSZ Go code generator (~1500 lines)
src/idl_gen_ssz_go.h      -- Generator header
go/ssz/                    -- Go runtime library
tests/ssz/                 -- Test schemas
```

## Limitations

- `uint16` array length limit (65535) prevents expressing some BeaconState fields (e.g. `EPOCHS_PER_HISTORICAL_VECTOR = 65536`)
- Fixed-length arrays (`[T:N]`) cannot appear directly in tables; wrap in a struct
- No schema evolution (SSZ requires all fields present, no optional/deprecated)

## Upstream

This is a fork of [google/flatbuffers](https://github.com/google/flatbuffers) with the SSZ code generation backend added. All existing FlatBuffers functionality is preserved.

## License

Apache License, Version 2.0. See [LICENSE](LICENSE).
