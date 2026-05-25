import SizzLean.Hasher.Class

/-!
# `SizzLean.Hasher.Sha256` — Day-1 SHA-256 `Hasher` instance via OpenSSL

Two `@[extern]` `opaque` declarations bridge Lean's `ByteArray`
to the C shim in `csrc/sha256_shim.c`, which wraps OpenSSL's
`EVP_*` API. The accompanying `Hasher Sha256` instance plugs both
into the abstract `Hasher` typeclass from
`SizzLean/Hasher/Class.lean`, so any downstream code with
`[Hasher Sha256]` in scope can pick this up at instance synthesis.

## File placement

Lives under `SizzLean/Hasher/` alongside `Hasher/Class.lean` (the
abstract interface) and `Hasher/Sha256Spec.lean` (the kernel-
reducible pure-Lean reference). All three follow the same
convention: the file path mirrors module path, but the namespace
inside is just `SizzLean` so the tag types and instances live at
`SizzLean.Sha256`, `SizzLean.Sha256Spec`, etc. — user-facing names
short, file layout grouped by topic.

## Trust boundary (ARCHITECTURE.md §9 / §11)

`opaque` keeps the kernel from attempting to reduce hash
computations during proof checking; `@[extern]` instructs the
compiler to emit a direct call to the named C symbol at runtime.
The trust assumption — *that the C shim implements NIST FIPS 180-4
SHA-256* — is validated empirically by
`SizzLeanTests/Sha256Vectors.lean` and is the single line item in
the TCB labelled "FFI SHA-256 assertion". A future
`@[csimp] theorem ffiSha256_eq_spec` would replace the empirical
assertion with a kernel-checked equality.

## Why the `Sha256` phantom tag

`class Hasher (H : Type)` carries `H` as a *phantom* type parameter —
it appears in the class binder but not in any method signature. Its
job is to disambiguate instances at the call site: `[Hasher Sha256]`
selects this OpenSSL-backed instance, leaving room for future
instances tagged `Poseidon2` or `Sha256Spec` (the pure-Lean reference
in `Hasher/Sha256Spec.lean`). Using an empty `inductive` for the
tag keeps it nominal — two distinct tag types resolve to two
distinct instances even if their implementations happen to coincide.

## Lean idioms used here

* `@[extern "C-symbol"] opaque foo : T` — declare `foo : T` such
  that the *runtime* implementation is the named C symbol, while
  the *kernel* treats `foo` as fully opaque (no reduction, no
  definitional equality with anything else). The pair is exactly
  what we want for an FFI primitive we don't want to reduce inside
  proofs.
* `@&` on a function argument marks it as *borrowed* — Lean's
  runtime does not bump the refcount when passing in. The C side
  receives a `b_lean_obj_arg` for these.
-/

set_option autoImplicit false

namespace SizzLean.Hasher

/-- Phantom tag for the FFI-backed SHA-256 `Hasher` instance.
Empty `inductive` so the type is nominal and distinct from any
future hash backend (e.g. `Sha256Spec`, the pure-Lean reference).
`Hasher.combine (H := Sha256) ...` selects this instance
unambiguously. -/
inductive Sha256 : Type

/-- 32-byte SHA-256 digest of an arbitrary-length input. Runtime
implementation is `csrc/sha256_shim.c`'s `lean_ssz_sha256_hash`.

The result is *always* 32 bytes (NIST FIPS 180-4 SHA-256 output
length); callers may rely on this as a documentation contract
enforced by the C shim, not by Lean's type system. -/
@[extern "lean_ssz_sha256_hash"]
opaque sha256Hash (input : @& ByteArray) : ByteArray

/-- 32-byte SHA-256 digest of `left ++ right` (concatenation),
without materialising the concatenation. Runtime implementation is
`csrc/sha256_shim.c`'s `lean_ssz_sha256_combine`.

Pulled out as its own primitive (rather than
`sha256Hash (left ++ right)`) so production instances can dispatch
directly to a SHA-NI / AVX-512 two-block hashing primitive without
a redundant copy at every interior tree node. The OpenSSL Day-1
backend just calls `EVP_DigestUpdate` twice — but the abstraction
is shaped for the eventual `gohashtree`-style upgrade. -/
@[extern "lean_ssz_sha256_combine"]
opaque sha256Combine (left right : @& ByteArray) : ByteArray

/-- The Day-1 `Hasher Sha256` instance — both methods delegate to
the FFI shim. -/
instance : Hasher Sha256 where
  hash    := sha256Hash
  combine := sha256Combine

end SizzLean.Hasher
