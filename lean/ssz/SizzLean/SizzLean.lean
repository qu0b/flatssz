import SizzLean.Repr.Class
import SizzLean.Repr.Instances
import SizzLean.Repr.Deriving
import SizzLean.Spec.SSZError
import SizzLean.Hasher.Class
import SizzLean.Hasher.Sha256
import SizzLean.Hasher.Sha256Spec
import SizzLean.Hasher.Sha256Equiv
import SizzLean.Hasher.Sha256Batch
import SizzLean.Cache.MerkleTree.HashCons
import SizzLean.Cache.TreeBacked
import SizzLean.Cache.Box
import SizzLean.Cache.Update

/-!
# `SizzLean` — library root

This file is the umbrella import. The re-exports above are the
library's *public surface* — names a user is expected to write in
their own code. They map one-to-one onto the sections of
[`MANUAL.md`](../MANUAL.md):

* `Repr/Class`, `Repr/Instances`, `Repr/Deriving` — the `SSZRepr`
  class, the built-in field-type instances (`Bool`, `UIntN`,
  `BitVec 128/256`, `Vector`, `SSZList`, `Bitvector`, `Bitlist`),
  and the `deriving SSZRepr` handler.
* `Spec/SSZError` — the deserialise-error sum returned by
  `SSZ.deserialize`.
* `Hasher/Class`, `Hasher/Sha256`, `Hasher/Sha256Spec` — the
  `Hasher` typeclass and its two shipping instances.
* `Hasher/Sha256Batch` — the batched FFI sibling-combine
  primitive (`sha256BatchCombine`) used by the cache layer's
  level-by-level builds.
* `Hasher/Sha256Equiv` — the named FFI ≡ pure-Lean equivalence
  axioms (`sha256Hash_eq_spec`, `sha256Combine_eq_spec`;
  `sha256BatchCombine_eq_spec` lives next to the batched
  primitive in `Sha256Batch`), rewrite-targets for proofs that
  need FFI hashes to reduce. The complete trust-boundary
  inventory is recoverable via `grep -rEn '^axiom |^@\[extern\]'`
  over `packages/SizzLean` — see the package README's
  "Trust assumptions you can grep for" section.
* `Cache/TreeBacked` — `CachedSSZ`, the cached-only one-flavour
  type, with `CachedSSZ.ofValue` and `CachedSSZ.hashTreeRoot`.
* `Cache/Box` — `SSZ.Box`, the closed union of cached + uncached
  flavours, plus the four smart constructors `SSZ.FastBox`,
  `SSZ.PureBox`, `SSZ.CachedBox`, `SSZ.UncachedBox`.
* `Cache/Update` — the `sszUpdate` macro.

Internal modules (the spec universe `SSZType` with its
serialise/deserialise/hashTreeRoot operations, the proof artefacts
in `Proofs/`, the Merkle-tree machinery in `Cache/MerkleTree/`,
the `UncachedSSZ` structure that backs `SSZ.PureBox` /
`SSZ.UncachedBox`) are *transitively* pulled in via the public
files above and remain importable by qualified path for advanced
uses or by sibling packages (`LeanEthCS` reaches into
`Spec/Serialize` etc. directly from its `deriving SSZRepr`
handler infrastructure). They are deliberately not listed here so
this file reads as the user's mental model of the library.

Acceptance / property-test gates live in a separate `lean_lib`
(`SizzLeanTests`); the default `lake build` skips them and they
fire via `lake build SizzLeanTests` (or `just test-ssz`).
-/
