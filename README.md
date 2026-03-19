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

**Rust (owned):**
```rust
let data = block.as_ssz_bytes();
let block = SignedBeaconBlock::from_ssz_bytes(&data)?;
let root = block.tree_hash_root();
```

**Rust (zero-copy view):**
```rust
let view = SignedBeaconBlockView::from_ssz_bytes(&data)?;  // 1.4ns — validates, no copy
let slot = view.message().slot();                           // reads directly from buffer
let root = view.message().tree_hash_root();                 // hashes from buffer
```

## Benchmarks

Deneb `SignedBeaconBlock` (~130KB), real Ethereum mainnet data, verified against known hash tree roots.

| Operation | Go | Rust (owned) | Rust (view) |
|---|---|---|---|
| **Unmarshal** | 31 us | **13 us** | **1.4 ns** |
| **Marshal** | 15 us | **3.8 us** | n/a (buffer is the encoding) |
| **HashTreeRoot** | 424 us | 424 us | 459 us |

Both Go and Rust use batch SIMD SHA-256 via [hashtree](https://github.com/OffchainLabs/hashtree). Rust views validate the full offset structure on construction (bounds, monotonicity, bools) and guarantee panic-free access — all for 1.4ns.

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
| `struct` / `table` | value struct | value struct + `View<'a>` |

## Project Structure

```
src/idl_gen_ssz_go.cpp      -- Go code generator
src/idl_gen_ssz_rust.cpp    -- Rust code generator (owned + zero-copy views)
go/ssz/                      -- Go runtime (error types)
rust/ssz_flatbuffers/        -- Rust runtime (Hasher via hashtree-rs, SszError)
tests/ssz/                   -- Test schemas and benchmarks
```

## Limitations

- Fixed-length arrays in tables must be wrapped in a struct
- `uint16` array length limit (65535) prevents some BeaconState fields
- No schema evolution (SSZ requires all fields present)

## License

Apache License, Version 2.0. Fork of [google/flatbuffers](https://github.com/google/flatbuffers).
