# flatssz

Cross-language SSZ code generation from [FlatBuffers](https://github.com/google/flatbuffers) schemas. Define Ethereum consensus layer types once in `.fbs` and generate code for 7 languages implementing marshal, unmarshal, and hash tree root. Supports EIP-7495 ProgressiveContainer and EIP-7916 ProgressiveList for forward-compatible schema evolution across forks. Go and Rust backends use [dynamic-ssz](https://github.com/pk910/dynamic-ssz)/[hashtree](https://github.com/OffchainLabs/hashtree) for SIMD-accelerated merkleization.

## Quick Start

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release && make -j
```

```bash
./flatc --ssz-go      -o output/ schema.fbs   # Go (geth)
./flatc --ssz-rust    -o output/ schema.fbs   # Rust (reth)
./flatc --ssz-ts      -o output/ schema.fbs   # TypeScript (Lodestar)
./flatc --ssz-zig     -o output/ schema.fbs   # Zig (Lantern)
./flatc --ssz-java    -o output/ schema.fbs   # Java (Besu/Teku)
./flatc --ssz-csharp  -o output/ schema.fbs   # C# (Nethermind)
./flatc --ssz-nim     -o output/ schema.fbs   # Nim (Nimbus)
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

// Zero-copy view — 1.4ns validated wrap, reads directly from buffer
let view = SignedBeaconBlockView::from_ssz_bytes(&data)?;
let slot = view.message().slot();
```

## Schema Evolution (EIP-7495 / EIP-7916)

Define types once with `ssz_progressive`. Add fields in future forks without breaking existing merkle proofs:

```fbs
table BeaconState (ssz_progressive) {
  genesis_time:ulong (id:0);       // Phase0
  slot:ulong (id:1);               // Phase0
  fork:Fork (id:2);                // Phase0
  // id:3 removed in a later fork  — gap in merkle tree
  sync_committee:SyncCommittee (id:4);  // Altair
  pending_consolidations:[PendingConsolidation] (id:5, ssz_max:"262144");  // Electra
}
```

Each field keeps a stable generalized index across all forks. Gaps produce zero-hash leaves. The active_fields bitvector is computed at codegen time. Serialization is identical to regular SSZ — only merkleization uses the progressive tree (subtrees of 1, 4, 16, 64, ... leaves).

Unbounded lists with progressive merkleization:

```fbs
table BeaconBlockBody {
  attestations:[Attestation] (ssz_progressive_list);  // no ssz_max needed
}
```

## Benchmarks

Deneb `SignedBeaconBlock` (~130KB), real Ethereum mainnet data, verified against known hash tree roots.

### Block operations

| Operation | Go | Rust | Rust (view) |
|---|---|---|---|
| **Unmarshal** | 31 us | **13 us** | **1.4 ns** |
| **Marshal** | 15 us | **3.8 us** | n/a |
| **HashTreeRoot** | 424 us | 424 us | 459 us |

### Progressive types

| Operation | Go | Rust |
|---|---|---|
| **ProgressiveContainer HTR** (4 fields, 1 gap) | 359 ns | 477 ns |
| **ProgressiveList HTR** (1000 uint64) | 9.3 us | 9.6 us |
| **ProgressiveList Marshal** (1000 uint64) | 1.6 us | 509 ns |

Both Go and Rust use batch SIMD SHA-256 via [hashtree](https://github.com/OffchainLabs/hashtree) (Go via cgo, Rust via [hashtree-rs](https://crates.io/crates/hashtree-rs)). Rust views validate the full offset structure on construction and guarantee panic-free access.

## SSZ Attributes

| Attribute | Purpose | Example |
|---|---|---|
| `ssz_max` | List capacity limit | `(ssz_max:"2048")` |
| `ssz_bitlist` | Bitlist type | `(ssz_bitlist, ssz_max:"2048")` |
| `ssz_bitvector` | Bitvector type | `(ssz_bitvector, ssz_bitsize:"64")` |
| `ssz_progressive` | EIP-7495 ProgressiveContainer | `table Foo (ssz_progressive)` |
| `ssz_progressive_list` | EIP-7916 ProgressiveList | `(ssz_progressive_list)` |
| `ssz_progressive_bitlist` | EIP-7916 ProgressiveBitlist | `(ssz_progressive_bitlist)` |

## Type Mapping

| FlatBuffers | Go | Rust |
|---|---|---|
| `bool` | `bool` | `bool` |
| `ubyte`..`ulong` | `uint8`..`uint64` | `u8`..`u64` |
| `[ubyte:32]` | `[32]byte` | `[u8; 32]` |
| `[T]` + `ssz_max` | `[]T` | `Vec<T>` |
| `[T]` + `ssz_progressive_list` | `[]T` | `Vec<T>` |
| `struct` / `table` | value struct | value struct + `View<'a>` |

## Project Structure

```
src/idl_gen_ssz_go.cpp      -- Go code generator
src/idl_gen_ssz_rust.cpp    -- Rust code generator (owned + zero-copy views)
src/idl_gen_ssz_ts.cpp      -- TypeScript code generator
src/idl_gen_ssz_zig.cpp     -- Zig code generator
src/idl_gen_ssz_java.cpp    -- Java code generator
src/idl_gen_ssz_csharp.cpp  -- C# code generator
src/idl_gen_ssz_nim.cpp     -- Nim code generator
go/ssz/                      -- Go runtime (error types)
rust/ssz_flatbuffers/        -- Rust runtime (Hasher, progressive merkleization)
tests/ssz/                   -- Test schemas and benchmarks
```

## Limitations

- Fixed-length arrays in tables must be wrapped in a struct
- `uint16` array length limit (65535) prevents some BeaconState fields
- ProgressiveContainer field IDs must be <= 255 (EIP-7495 limit)

## License

Apache License, Version 2.0. Fork of [google/flatbuffers](https://github.com/google/flatbuffers).
