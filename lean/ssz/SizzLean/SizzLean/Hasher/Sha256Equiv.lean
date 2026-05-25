import SizzLean.Hasher.Sha256
import SizzLean.Hasher.Sha256Spec
import LeanSha256.Core

/-!
# `SizzLean.Hasher.Sha256Equiv` — FFI / spec SHA-256 equivalence axiom

## What this file asserts

Two axioms stating that the FFI-backed `sha256Hash` and
`sha256Combine` (in `Hasher/Sha256.lean`) compute the same function
on every input as the pure-Lean reference (`LeanSha256.hash` and
`LeanSha256.combine`).

```lean
axiom sha256Hash_eq_spec    : @sha256Hash    = LeanSha256.hash
axiom sha256Combine_eq_spec : @sha256Combine = LeanSha256.combine
```

## Why axioms (and not theorems)

The FFI implementation lives in `csrc/sha256_shim.c` and calls
OpenSSL's `EVP_*`. Proving in Lean that the C code computes SHA-256
would require extracting the C semantics into Lean — not feasible
without heavy machinery (verified C compiler, model of OpenSSL's
internals, etc.).

We instead:

1. **Validate the equivalence empirically.** `Conformance/Sha256Vectors.lean`
   runs the FFI against NIST CAVP vectors;
   `Conformance/Sha256Equivalence.lean` checks that the FFI and the
   pure-Lean reference agree on a randomised input batch. Both gates
   build under `lake build SizzLeanTests`.
2. **Promote that validation to a named Lean axiom here.** Proofs
   can now `rw` the FFI calls into their pure-Lean equivalents — at
   audit time, `#axioms theoremName` lists `sha256Hash_eq_spec` /
   `sha256Combine_eq_spec` as the (single, named, replaceable) trust
   assumptions behind the proof.

The trust commitment is the empirical assertion "the FFI shim
implements SHA-256," already validated by the CAVP conformance
tests. Naming it as a Lean axiom makes the assumption visible in
`#axioms` and replaceable in one place when the corresponding
`@[csimp]` proof lands.

## How proofs use these

The typical pattern is to rewrite Sha256-flavoured terms into
Sha256Spec-flavoured ones inside a proof, then close with
`native_decide` (which trusts the compiler's reduction of pure-Lean
code rather than the C shim):

```lean
theorem someBeaconStateRoot :
    (SSZ.FastBox myState).hashTreeRoot = expectedHex := by
  rw [show @sha256Hash    = LeanSha256.hash    from sha256Hash_eq_spec]
  rw [show @sha256Combine = LeanSha256.combine from sha256Combine_eq_spec]
  -- Now every FFI call has been substituted for its pure-Lean
  -- equivalent. The term is fully kernel-evaluable; native_decide
  -- closes via compiled code-gen of LeanSha256.
  native_decide
```

For pure state-transition proofs that don't need concrete hash
values (only structural equalities), these axioms are *not*
required — both sides of the equality invoke the same opaque
`sha256Hash` / `sha256Combine` calls and `rfl` / `simp` closes
them without ever caring what the bytes are. Reach for these
axioms only when a goal requires the hash to *actually compute*.

## Future hardening

A planned `@[csimp]`-attributed equality between the FFI and the
spec hashers would replace these axioms with theorems carrying
the same statements; every dependent proof tightens automatically
— no theorem statements need to change.
-/

set_option autoImplicit false

namespace SizzLean.Hasher

/-- **Axiom**: the FFI-backed `sha256Hash` (which calls
`csrc/sha256_shim.c`'s `lean_ssz_sha256_hash` via `@[extern]`)
computes the same function as `LeanSha256.hash` (the pure-Lean
NIST-validated reference). Empirically validated by
`Conformance.Sha256Vectors` + `Conformance.Sha256Equivalence`;
promoted here to a named Lean axiom so proofs that depend on it
can be audited via `#axioms`.

Replaceable by a `@[csimp]`-proved theorem when one lands; proof
shapes stay identical. -/
axiom sha256Hash_eq_spec : @sha256Hash = LeanSha256.hash

/-- **Axiom**: the FFI-backed `sha256Combine` (which calls
`csrc/sha256_shim.c`'s `lean_ssz_sha256_combine`) computes
SHA-256 over the concatenation of its two inputs, matching
`LeanSha256.combine`'s pure-Lean implementation. Same validation /
auditing story as `sha256Hash_eq_spec`. -/
axiom sha256Combine_eq_spec : @sha256Combine = LeanSha256.combine

/-! ### Smoke test: the rewrite closes a Sha256 → Sha256Spec goal

A minimal proof that the axioms can be used to convert an FFI hash
call into its pure-Lean equivalent. If either axiom name or
statement drifts, this example stops elaborating. The `#axioms`
inspection below shows the named axiom dependency. -/

example (b : ByteArray) : sha256Hash b = LeanSha256.hash b := by
  rw [sha256Hash_eq_spec]

example (l r : ByteArray) : sha256Combine l r = LeanSha256.combine l r := by
  rw [sha256Combine_eq_spec]

end SizzLean.Hasher
