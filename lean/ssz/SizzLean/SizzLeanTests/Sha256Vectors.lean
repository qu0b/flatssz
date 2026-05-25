import SizzLean.Hasher.Sha256

/-!
# `SizzLeanTests.Sha256Vectors` — NIST CAVP smoke test

Canonical SHA-256 test vectors validated against the FFI shim
via `native_decide` (per ARCHITECTURE.md §9.1 / §11: each
`native_decide` invocation adds a `Lean.ofReduceBool` axiom; the
conformance suite path is allowed to use them, the proof path is
not).

The selected vectors come from the NIST FIPS 180-4 specification's
worked examples plus the common SSZ Merkleization smoke test
(`combine 32×0x00 32×0x00`). Together they exercise:

* Empty input (`""`) — degenerate length case.
* Short input (`"abc"`) — the canonical 24-bit test.
* Multi-block input (56-byte input forces SHA-256's
  Merkle–Damgård padding to span two blocks).
* `combine zero32 zero32` — the SSZ Merkleization zero-pair, the
  base of `zeroHashAt 1` in the cache layer's zero-hash table.
* `combine zero32 (sha256Hash "")` — cross-check between the two
  primitives (`sha256Hash`'s "" output as the right operand of
  `combine` should equal `sha256Hash (zero32 ++ "")`).

A correct C shim passes all five; a wrong shim (truncation, byte
order, padding bug) fails at least one. The vectors are tiny and
hard-coded so that adding a new one is a one-line constant plus a
one-line `example`.

## Lean idioms used here

* `ByteArray.mk #[0xab, 0xcd, …]` — build a `ByteArray` from a
  literal `Array UInt8`. `UInt8` literals support `0x` hex
  notation.
* `native_decide` — evaluate the (closed) proposition by running
  compiled code at proof-check time. Each invocation introduces a
  `Lean.ofReduceBool` axiom keyed on the proposition; these
  *implementation* axioms are outside the verified-by-induction
  trusted core, but inside the FFI-empirical-validation trusted
  base (ARCHITECTURE.md §11).
-/

set_option autoImplicit false

namespace SizzLeanTests.Sha256Vectors

open SizzLean.Hasher

open SizzLean

/-! ### Test inputs -/

/-- 32 zero bytes — the SSZ "zero leaf" and the most common
`combine` operand in Merkleization. -/
def zero32 : ByteArray := ByteArray.mk <| Array.replicate 32 0

/-- The 56-byte FIPS 180-4 worked example input
`abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq`. Exactly
this length so padding spans into a second block. -/
def fips56 : ByteArray :=
  String.toUTF8 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"

/-! ### Expected digests

All from FIPS 180-4 §B / NIST CAVP test vectors. -/

/-- `SHA-256("")` per FIPS 180-4 §B.0:
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. -/
def expected_empty : ByteArray := ByteArray.mk #[
  0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
  0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
  0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
  0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55]

/-- `SHA-256("abc")` per FIPS 180-4 §B.1:
`ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`. -/
def expected_abc : ByteArray := ByteArray.mk #[
  0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
  0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
  0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad]

/-- `SHA-256("abcdbcdec...")` per FIPS 180-4 §B.2 (the 56-byte
two-block example):
`248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1`. -/
def expected_fips56 : ByteArray := ByteArray.mk #[
  0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
  0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
  0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
  0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1]

/-- `SHA-256(zero32 ++ zero32)` — the SSZ `ZERO_HASHES[1]` seed:
`f5a5fd42d16a20302798ef6ed309979b43003d2320d9f0e8ea9831a92759fb4b`. -/
def expected_zero_combine : ByteArray := ByteArray.mk #[
  0xf5, 0xa5, 0xfd, 0x42, 0xd1, 0x6a, 0x20, 0x30,
  0x27, 0x98, 0xef, 0x6e, 0xd3, 0x09, 0x97, 0x9b,
  0x43, 0x00, 0x3d, 0x23, 0x20, 0xd9, 0xf0, 0xe8,
  0xea, 0x98, 0x31, 0xa9, 0x27, 0x59, 0xfb, 0x4b]

/-! ### Vectors

Each `example` is closed by `native_decide`: the FFI is invoked at
compile time, the result compared to the constant, and the
proposition is reduced to `True`. A wrong C shim surfaces here. -/

/-- `sha256Hash ""` matches FIPS 180-4 §B.0. -/
example : sha256Hash ByteArray.empty = expected_empty := by native_decide

/-- `sha256Hash "abc"` matches FIPS 180-4 §B.1. -/
example : sha256Hash (String.toUTF8 "abc") = expected_abc := by native_decide

/-- `sha256Hash` on the 56-byte multi-block input matches §B.2. -/
example : sha256Hash fips56 = expected_fips56 := by native_decide

/-- `sha256Combine zero32 zero32` matches the SSZ `ZERO_HASHES[1]`. -/
example : sha256Combine zero32 zero32 = expected_zero_combine := by native_decide

/-- Cross-check: `combine` of `(left, right)` should equal `hash` of
the concatenation. This catches shim bugs where the two-input path
diverges from the single-input path. -/
example :
    sha256Combine ByteArray.empty (sha256Hash ByteArray.empty) =
      sha256Hash (ByteArray.empty ++ sha256Hash ByteArray.empty) := by
  native_decide

end SizzLeanTests.Sha256Vectors
