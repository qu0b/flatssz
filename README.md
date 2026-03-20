# flatssz

Cross-language SSZ code generation from [FlatBuffers](https://github.com/google/flatbuffers) schemas. Define Ethereum consensus layer types once in `.fbs` and generate code for 7 languages. Supports EIP-7495 ProgressiveContainer and EIP-7916 ProgressiveList for forward-compatible schema evolution across forks.

## Supported Languages

| Flag | Language | Client | Features |
|---|---|---|---|
| `--ssz-go` | Go | geth | marshal, unmarshal, HTR, progressive |
| `--ssz-rust` | Rust | reth | marshal, unmarshal, HTR, progressive, zero-copy views |
| `--ssz-ts` | TypeScript | Lodestar | marshal, unmarshal, HTR, progressive |
| `--ssz-zig` | Zig | Lantern | marshal, unmarshal, HTR, progressive |
| `--ssz-java` | Java | Besu/Teku | marshal, unmarshal, HTR, progressive |
| `--ssz-csharp` | C# | Nethermind | marshal, unmarshal, HTR, progressive |
| `--ssz-nim` | Nim | Nimbus | marshal, unmarshal, HTR, progressive |

## Quick Start

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release && make -j
./flatc --ssz-go --ssz-rust --ssz-ts --ssz-zig --ssz-java --ssz-csharp --ssz-nim -o output/ schema.fbs
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

### Go
```go
data, _ := block.MarshalSSZ()
block.UnmarshalSSZ(data)
root, _ := block.HashTreeRoot()
```

### Rust
```rust
let block = SignedBeaconBlock::from_ssz_bytes(&data)?;
let data = block.as_ssz_bytes();
let root = block.tree_hash_root();

// Zero-copy view — 1.2ns validated wrap
let view = SignedBeaconBlockView::from_ssz_bytes(&data)?;
let slot = view.message().slot();
```

### TypeScript
```typescript
const block = SignedBeaconBlock.fromSszBytes(data);
const encoded = block.toSszBytes();
const root = block.treeHashRoot();
```

### Zig
```zig
const block = try SignedBeaconBlock.fromSszBytes(data);
const encoded = try block.toSszBytes(allocator);
const root = block.treeHashRoot();
```

### Java
```java
SignedBeaconBlock block = SignedBeaconBlock.fromSSZBytes(data);
byte[] encoded = block.marshalSSZ();
byte[] root = block.hashTreeRoot();
```

### C#
```csharp
var block = SignedBeaconBlock.FromSSZBytes(data);
byte[] encoded = block.MarshalSSZ();
byte[] root = block.HashTreeRoot();
```

### Nim
```nim
let block = SignedBeaconBlock.fromSSZBytes(data)
let encoded = block.marshalSSZ()
let root = block.hashTreeRoot()
```

## Schema Evolution (EIP-7495 / EIP-7916)

Add fields in future forks without breaking existing merkle proofs:

```fbs
table BeaconState (ssz_progressive) {
  genesis_time:ulong (id:0);       // Phase0
  slot:ulong (id:1);               // Phase0
  fork:Fork (id:2);                // Phase0
  // id:3 removed — gap in merkle tree
  sync_committee:SyncCommittee (id:4);  // Altair
  pending_consolidations:[PendingConsolidation] (id:5, ssz_max:"262144");  // Electra
}
```

Each field keeps a stable generalized index across all forks. Serialization is unchanged — only merkleization uses the progressive tree.

## Benchmarks

Deneb `SignedBeaconBlock` (~130KB), real Ethereum mainnet data, verified against known hash tree roots.

| Operation | Go | Rust | Rust (view) | TypeScript |
|---|---|---|---|---|
| **Unmarshal** | 32 us | **12 us** | **1.2 ns** | 497 us |
| **Marshal** | 16 us | **3.6 us** | — | 82 us |
| **HashTreeRoot** | **399 us** | 455 us | — | 23,913 us |

Go and Rust use batch SIMD SHA-256 via [hashtree](https://github.com/OffchainLabs/hashtree). TypeScript uses a pure-JS SHA-256 implementation — the 60x HTR gap is entirely the hash function, not the codegen.

| Progressive | Go | Rust |
|---|---|---|
| **Container HTR** (4 fields, 1 gap) | 398 ns | 501 ns |
| **List HTR** (1000 uint64) | 10.8 us | 10.3 us |
| **List Marshal** (1000 uint64) | 1.3 us | **505 ns** |

## SSZ Attributes

| Attribute | Purpose | Example |
|---|---|---|
| `ssz_max` | List capacity limit | `(ssz_max:"2048")` |
| `ssz_bitlist` | Bitlist type | `(ssz_bitlist, ssz_max:"2048")` |
| `ssz_bitvector` | Bitvector type | `(ssz_bitvector, ssz_bitsize:"64")` |
| `ssz_progressive` | EIP-7495 ProgressiveContainer | `table Foo (ssz_progressive)` |
| `ssz_progressive_list` | EIP-7916 ProgressiveList | `(ssz_progressive_list)` |

## Limitations

- Fixed-length arrays in tables must be wrapped in a struct
- `uint16` array length limit (65535) prevents some BeaconState fields
- ProgressiveContainer field IDs must be <= 255 (EIP-7495 limit)

## License

Apache License, Version 2.0. Fork of [google/flatbuffers](https://github.com/google/flatbuffers).
