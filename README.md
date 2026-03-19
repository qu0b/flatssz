# flatssz

Cross-language SSZ code generation from [FlatBuffers](https://github.com/google/flatbuffers) schemas. Define Ethereum consensus layer types once in `.fbs` and generate Go and Rust code implementing marshal, unmarshal, and hash tree root. Go backend uses [dynamic-ssz](https://github.com/pk910/dynamic-ssz) for SIMD-accelerated merkleization.

## Quick Start

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release && make -j
```

```bash
./flatc --ssz-go   -o output/ schema.fbs   # Go
./flatc --ssz-rust -o output/ schema.fbs   # Rust
```

```fbs
struct Checkpoint {
  epoch:ulong;
  root:[ubyte:32];
}

table Attestation {
  aggregation_bits:[ubyte] (ssz_bitlist, ssz_max:"2048");
  data:AttestationData;
  signature:Bytes96;
}
```

**Go:**
```go
data, _ := block.MarshalSSZ()
block.UnmarshalSSZ(data)
root, _ := block.HashTreeRoot()
```

**Rust:**
```rust
let data = block.as_ssz_bytes();
let block = SignedBeaconBlock::from_ssz_bytes(&data)?;
let root = block.tree_hash_root();
```

## Benchmarks

Deneb `SignedBeaconBlock` (~130KB), real Ethereum mainnet data, verified against known hash tree roots.

| Operation | Go | Rust |
|---|---|---|
| **Unmarshal** | 31.3 us | **12.2 us** (2.6x) |
| **Marshal** | 15.5 us | **3.7 us** (4.2x) |
| **HashTreeRoot** | **413 us** (1.4x) | 589 us |

Go HTR uses batch SIMD SHA-256 via [hashtree](https://github.com/prysmaticlabs/hashtree) (cgo). Rust uses `sha2` with SHA-NI intrinsics (inlined, no FFI).

## SSZ Attributes

| Attribute | Purpose | Example |
|---|---|---|
| `ssz_max` | List capacity limit | `(ssz_max:"2048")` |
| `ssz_bitlist` | Bitlist type | `(ssz_bitlist, ssz_max:"2048")` |
| `ssz_bitvector` | Bitvector type | `(ssz_bitvector, ssz_bitsize:"64")` |

Comma-separated for nested lists: `(ssz_max:"1048576,1073741824")`.

## Type Mapping

| FlatBuffers | Go | Rust |
|---|---|---|
| `bool` | `bool` | `bool` |
| `ubyte`..`ulong` | `uint8`..`uint64` | `u8`..`u64` |
| `[ubyte:32]` | `[32]byte` | `[u8; 32]` |
| `[T]` + `ssz_max` | `[]T` | `Vec<T>` |
| `[string]` + `ssz_max` | `[][]byte` | `Vec<Vec<u8>>` |
| `struct` / `table` | value struct | value struct |

## Project Structure

```
src/idl_gen_ssz_go.cpp      -- Go code generator
src/idl_gen_ssz_rust.cpp    -- Rust code generator
go/ssz/                      -- Go runtime (error types)
rust/ssz_flatbuffers/        -- Rust runtime (Hasher, SszError)
tests/ssz/                   -- Test schemas and benchmarks
```

## Limitations

- Fixed-length arrays in tables must be wrapped in a struct
- `uint16` array length limit (65535) prevents some BeaconState fields
- No schema evolution (SSZ requires all fields present)

## License

Apache License, Version 2.0. Fork of [google/flatbuffers](https://github.com/google/flatbuffers).
