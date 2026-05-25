import SizzLean.Spec.Type
import SizzLean.Spec.Serialize  -- for isFixedSize / allFixedSize

/-!
# `SizzLean.Spec.BasicSupported` — the predicate the proof set grows over

A *strict subset* of `SSZType.Supported` (in `Spec/Supported.lean`)
that the three central theorems (`decode_encode`,
`serialize_injective`, `encode_size_le_max`) are currently proved
for. Each constructor here names an `SSZType` shape on which the
proofs close exhaustively; adding a constructor obliges the
proofs to extend. The predicate can be retired entirely once the
proofs cover all of `Supported`.

The predicate lives in `Spec/` (not `Proofs/`) because the
user-facing `SSZ.roundtrip` corollary in `Repr/Class.lean`
mentions it — a layering concern that follows ARCHITECTURE.md §2's
library-then-surface flow (Spec layer below, Repr layer above;
Proofs/ reaches over to discharge the theorems).

## Coverage (current)

* **Basic integers**: `.uintN 8 / 16 / 32 / 64` (closed in
  `Proofs/UInt.lean` via `unfold` + `bv_decide`).
* **Bool**: `.bool` (closed by `cases`, in `Proofs/Bool.lean`).
* **Composites**: `.vector t n` / `.list t cap` /
  `.container fs` over fixed-size element / field types
  (closed in `Proofs/{VectorFixed,ListFixed,ContainerFixed}.lean`
  via mutual induction with the shared prereq
  `Proofs/SerializeSize.lean`).

## Deferred (still outside `BasicSupported`)

* **Bitvector** (`.bitvector n` with `0 < n`) — bit-packing
  inverse `packBitsLE_unpackBitsLEAux_inverse` not yet shipped.
* **Bitlist** (`.bitlist cap`) — adds `msbPos` delimiter
  recovery on top of bitvector's prerequisites.

Both arms remain `Supported` (the spec implements them and
conformance passes), but the `decode_encode` corollary is
ungated for these shapes pending the bit-packing proofs.

## Why two mutually inductive predicates

The general `.container fs` arm needs to *recurse* into its field
list — each field must itself be `BasicSupported` and fixed-size.
`BasicSupportedFieldsFixed` captures this pointwise; it is mutual
with `BasicSupported` because the field-list predicate's `cons`
constructor takes a `BasicSupported t` witness for the head.

## Why `0 < n` on `vectorFixed` / `bitvector`

The spec rejects `n = 0` at *decode* time for both shapes
(`ssz_generic/basic_vector/invalid/vec_*_0` and
`ssz_generic/bitvector/invalid/bitvec_0` test cases), so the
universal roundtrip would fail in those constructors. We carry
the precondition only at the `BasicSupported` layer rather than
tightening `Supported` itself — the latter is a more invasive
spec adjustment that PLAN.md's *full* Stage 18 closure will need
to do anyway, but it can wait.

-/

set_option autoImplicit false

namespace SizzLean.Spec

mutual
/-- Narrow correctness-coverage predicate. Each constructor names
an `SSZType` shape for which all three central theorems
(`decode_encode`, `serialize_injective`, `encode_size_le_max`) are
proved in `Proofs/`. Adding a constructor obliges the proofs to
extend; the planned generalisation widens this to all of
`SSZType.Supported`. -/
inductive SSZType.BasicSupported : SSZType → Prop
  /-- Single-byte unsigned integer. `serialize` is `empty.push x`;
  the roundtrip closes by `rfl` after one `unfold`. -/
  | uintN8 : SSZType.BasicSupported (.uintN 8)
  /-- 16-bit little-endian unsigned integer. Closes via the
  per-byte indexing chain reduced by `rfl` + `bv_decide` on the
  residual LE identity. -/
  | uintN16 : SSZType.BasicSupported (.uintN 16)
  /-- 32-bit little-endian unsigned integer. -/
  | uintN32 : SSZType.BasicSupported (.uintN 32)
  /-- 64-bit little-endian unsigned integer. -/
  | uintN64 : SSZType.BasicSupported (.uintN 64)
  /-- `Bool` — single-byte 0/1. -/
  | bool : SSZType.BasicSupported .bool
  /-- Fixed-length vector with fixed-size element type and
  non-empty length. The `n > 0` precondition mirrors the spec's
  zero-length rejection. -/
  | vectorFixed : ∀ {t : SSZType} {n : Nat},
                  0 < n → SSZType.BasicSupported t → t.isFixedSize = true →
                  SSZType.BasicSupported (.vector t n)
  /-- Variable-length list (up to `cap`) with fixed-size element
  type and positive element size. The `0 < t.fixedByteSize`
  precondition rules out the `.container []`-element pathology
  where the spec's `if sz = 0 then .error .tooShort` decoder
  guard would fail. -/
  | listFixed : ∀ {t : SSZType} {cap : Nat},
                SSZType.BasicSupported t → t.isFixedSize = true →
                0 < t.fixedByteSize →
                SSZType.BasicSupported (.list t cap)
  /-- Container with an all-fixed-size, all-`BasicSupported`
  field list. -/
  | containerFixed : ∀ {fs : List SSZType},
                     SSZType.BasicSupportedFieldsFixed fs →
                     SSZType.BasicSupported (.container fs)

/-- Pointwise `BasicSupported ∧ isFixedSize` over a field list.
Used by the `containerFixed` arm. The `isFixedSize` half makes
the container decoder's `allFixedSize fs` guard pass; the
`BasicSupported` half lets the per-field roundtrip recurse. -/
inductive SSZType.BasicSupportedFieldsFixed : List SSZType → Prop
  | nil : SSZType.BasicSupportedFieldsFixed []
  | cons : ∀ {t : SSZType} {ts : List SSZType},
           SSZType.BasicSupported t → t.isFixedSize = true →
           SSZType.BasicSupportedFieldsFixed ts →
           SSZType.BasicSupportedFieldsFixed (t :: ts)
end

end SizzLean.Spec
