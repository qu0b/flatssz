import SizzLean.Hasher.Class
import SizzLean.Hasher.Sha256
import LeanSha256.Core

/-!
# `SizzLean.Hasher.Sha256Batch` — batched FFI SHA-256 sibling combine

A level-batched variant of `sha256Combine`: given two equal-length
arrays of sibling byte-strings (typically 32 bytes each — the
Merkle interior-node case), produce one array of SHA-256 digests
of the pointwise concatenations.

The runtime implementation is `csrc/sha256_batch.c`'s
`lean_ssz_sha256_batch_combine`. The initial cut is a plain loop
over the existing scalar `EVP_DigestUpdate`-twice path with one
shared `EVP_MD_CTX` reused across pairs — modest constant-factor
win over per-pair allocation. The intent is to swap the inner
loop later for a SHA-NI / AVX-512 implementation along the lines
of `gohashtree`'s `sha256_avx_x4` / `sha256_avx512_x16` without
changing the FFI surface.

## Wire format

```lean
@[extern "lean_ssz_sha256_batch_combine"]
opaque sha256BatchCombine
    (lefts : @& Array ByteArray) (rights : @& Array ByteArray) :
    Array ByteArray
```

Inputs are *parallel* arrays — `lefts[i]` is the left sibling at
position `i`, `rights[i]` is the right. Output is a same-length
array; `output[i]` is `SHA-256(lefts[i] ++ rights[i])`. The C
shim panics if the input lengths disagree.

## Trust footprint

One new named axiom — `sha256BatchCombine_eq_spec` — assertin
that the FFI primitive agrees pointwise with the pure-Lean
SHA-256 reference (`LeanSha256.combine`). Empirically validated
by `SizzLeanTests/Sha256BatchEquivalence.lean` (mirrors the
existing scalar validation in `Sha256Equivalence.lean`). A future
`@[csimp]`-proved theorem would clear this axiom alongside the
scalar ones.

The trust commitment is the same shape as the existing
`sha256Hash_eq_spec` / `sha256Combine_eq_spec` pair — we trust
OpenSSL plus the C shim's loop. The empirical validation pins
that trust to a specific input class (parallel arrays of byte
strings).

## Why a separate primitive, not a wrapper

A pure-Lean wrapper

```lean
def batchCombine (lefts rights : Array ByteArray) : Array ByteArray :=
  (lefts.zip rights).map fun (l, r) => sha256Combine l r
```

would still pay one C call per pair plus one `EVP_MD_CTX_new` /
`EVP_MD_CTX_free` per pair. The batched primitive amortises the
context allocation across the entire pair array, and leaves room
for a SHA-NI / AVX-512 inner loop that processes multiple pairs
per instruction. The wrapper form stays available — see the
`sha256BatchCombine_eq_spec` axiom below — but is not the
performance-critical path.
-/

set_option autoImplicit false

namespace SizzLean.Hasher

/-- Batched FFI SHA-256 sibling combine. Inputs are parallel
arrays — `lefts[i]` and `rights[i]` are the i-th sibling pair —
and the output array has the same length, with `output[i] =
SHA-256(lefts[i] ++ rights[i])`. The C shim panics if the input
lengths disagree.

Runtime implementation is `csrc/sha256_batch.c`'s
`lean_ssz_sha256_batch_combine` — currently a scalar loop with
a shared `EVP_MD_CTX`, swappable for SHA-NI / AVX-512 later
without changing the FFI surface. -/
@[extern "lean_ssz_sha256_batch_combine"]
opaque sha256BatchCombine
    (lefts : @& Array ByteArray) (rights : @& Array ByteArray) :
    Array ByteArray

/-- The pure-Lean reference shape that `sha256BatchCombine`
matches pointwise. Stated as a `def` so the axiom below can name
the equality cleanly. -/
def sha256BatchCombineSpec
    (lefts rights : Array ByteArray) : Array ByteArray :=
  (lefts.zip rights).map fun (l, r) => LeanSha256.combine l r

/-- **Axiom**: the FFI batched primitive computes the same
function on every input as the pure-Lean reference. Empirically
validated by `Sha256BatchEquivalence.lean`; promoted here to a
named Lean axiom so proofs that depend on the batched primitive
can be audited via `#axioms`. Same trust footprint as
`sha256Combine_eq_spec` — we trust OpenSSL plus the C shim's
loop. Replaceable by a `@[csimp]`-proved theorem when one lands;
proof shapes stay identical. -/
axiom sha256BatchCombine_eq_spec :
    @sha256BatchCombine = sha256BatchCombineSpec

/-! ### Smoke test: rewrite closes a batched-FFI goal -/

example (ls rs : Array ByteArray) :
    sha256BatchCombine ls rs = sha256BatchCombineSpec ls rs := by
  rw [sha256BatchCombine_eq_spec]

end SizzLean.Hasher
