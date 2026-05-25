import SizzLean.Hasher.Class
import SizzLean.Spec.Type
import SizzLean.Spec.Interp
import SizzLean.Spec.SSZError
import SizzLean.Spec.Serialize
import SizzLean.Spec.Deserialize
import SizzLean.Spec.HashTreeRoot
import SizzLean.Spec.BasicSupported
import SizzLean.Proofs.Roundtrip

/-!
# `SizzLean.Repr.Class` — `SSZRepr` typeclass + thin user-facing wrappers

The `SSZRepr` class declaration plus the `SSZ.serialize` /
`SSZ.deserialize` / `SSZ.hashTreeRoot` user-facing wrappers and
the `SSZ.roundtrip` per-user-type corollary.

ARCHITECTURE.md §5.1 specifies the class:

```
class SSZRepr (T : Type) where
  shape    : SSZType
  toRepr   : T → shape.interp
  fromRepr : shape.interp → T
  to_from  : ∀ x, fromRepr (toRepr x) = x
  from_to  : ∀ r, toRepr (fromRepr r) = r
```

A `SSZRepr T` instance carries a *shape* (the `SSZType` description
that classifies `T`'s wire format), an isomorphism between `T` and
that shape's interpretation, and proofs that the isomorphism is
genuine. Per-user-type `serialize` / `deserialize` / `hashTreeRoot`
are then thin wrappers; `SSZ.roundtrip` lifts the spec-side
`decode_encode` (`Proofs/Roundtrip.lean`) to the user type via the
`from_to` law.

## Why `SSZ.roundtrip` is gated by `BasicSupported r.shape`

The library's `decode_encode` proof currently covers the
`BasicSupported` subset:

* `.uintN 8 / 16 / 32 / 64`, `.bool` — basic primitives.
* `.vector t n` / `.list t cap` / `.container fs` — general
  composites over fixed-size element / field types
  (`BasicSupported t` with `t.isFixedSize = true`).
* `.container [.bool, .bool]` — concrete two-`Bool` container,
  retained as a `def`-level alias of `containerFixed (.cons .bool
  rfl (.cons .bool rfl .nil))` for backward-compat with the
  hand-written `Pair` example.

The user-surface corollary inherits that gate: a user type whose
shape sits inside `BasicSupported` enjoys verified roundtrip; a
user type whose shape is outside `BasicSupported` (currently
`.bitvector` and `.bitlist` — both pending the bit-packing
inverse proof) enjoys total `serialize` / `deserialize` (the
spec functions are total) but no verified roundtrip yet. Wider
proof coverage is planned future work; until it lands the gate is
honest about scope and grows automatically as the proof set
widens.

## Lean idioms used here

* `class C T where` — typeclass declaration; instances are
  resolved at call sites via `[C T]` instance binders.
* `Hasher.hash`/`Hasher.combine` arguments resolved by `(H := H)`
  named arguments — `H` is a phantom tag with no occurrences in
  the methods' types, so instance synthesis can't recover it from
  value arguments. Same idiom as `Spec/HashTreeRoot.lean`.
-/

set_option autoImplicit false

namespace SizzLean

open SizzLean.Spec

/-- `SSZRepr T` — the user-facing typeclass bridging Lean types to
the SSZ wire format.

A type `T` carries an `SSZRepr T` instance by exhibiting:
* `shape` — the `SSZType` description that classifies `T`'s wire
  format (e.g. `.uintN 64` for a `Slot`-like wrapper, or
  `.container [.bool, .bool]` for a `Pair {a b : Bool}`).
* `toRepr` / `fromRepr` — a per-direction conversion between `T`
  values and `shape.interp` values. These are *value-level* iso
  arrows: at runtime they perform any boxing / unboxing the
  in-memory representation requires.
* `to_from` / `from_to` — the iso laws, kept on the class itself
  (not as separate theorems) so a user-supplied instance must
  commit to them at definition time. For library-provided
  instances and `deriving`-generated instances, both laws close
  by `rfl` because the iso is definitionally the identity.
-/
class SSZRepr (T : Type) where
  /-- The `SSZType` description classifying `T`'s wire format. -/
  shape    : SSZType
  /-- Forward iso: `T` → wire-form value. -/
  toRepr   : T → shape.interp
  /-- Inverse iso: wire-form value → `T`. -/
  fromRepr : shape.interp → T
  /-- Iso law (left): `fromRepr ∘ toRepr = id`. -/
  to_from  : ∀ x, fromRepr (toRepr x) = x
  /-- Iso law (right): `toRepr ∘ fromRepr = id`. The user-facing
  `SSZ.roundtrip` corollary uses this direction to convert the
  spec-level round-tripped value `toRepr (fromRepr y)` back to
  plain `y`. -/
  from_to  : ∀ r, toRepr (fromRepr r) = r

namespace SSZ

/-- User-facing serializer. Delegates to the spec-level `serialize`
through the `SSZRepr` instance's shape and forward iso.

`@[specialize]` lets the compiler monomorphise this at each
consensus type that calls it (`Validator`, `BeaconBlockHeader`,
…), removing the `SSZRepr`-instance dispatch at the hot path's
call site. The kernel still sees the unspecialised definition
for proof reduction. -/
@[specialize]
def serialize {T : Type} [r : SSZRepr T] (x : T) : ByteArray :=
  SSZType.serialize r.shape (r.toRepr x)

/-- User-facing deserializer. Decodes against the instance's shape;
on success, converts back through `fromRepr`; on failure, propagates
the `SSZError`. `@[specialize]` per `serialize` above. -/
@[specialize]
def deserialize {T : Type} [r : SSZRepr T] (b : ByteArray) :
    Except SSZError T :=
  match SSZType.deserialize r.shape b with
  | .ok (y, _) => .ok (r.fromRepr y)
  | .error e   => .error e

/-- User-facing Merkleization. Delegates to the spec-level
`hashTreeRoot` through the `Hasher` instance and the `SSZRepr`'s
shape. The `(H := H)` is needed because `Hasher`'s parameter is a
phantom tag — same idiom as `Spec/HashTreeRoot.lean`.
`@[specialize]` per `serialize` above. -/
@[specialize]
def hashTreeRoot {T : Type} (H : Type) [Hasher H] [r : SSZRepr T]
    (x : T) : ByteArray :=
  Spec.hashTreeRoot (H := H) r.shape (r.toRepr x)

/-- Per-user-type roundtrip corollary.

Given `[SSZRepr T]` with `BasicSupported r.shape`, the
`SSZ.deserialize ∘ SSZ.serialize` round-trip returns `.ok x` for any
`x : T`.

The proof unfolds the wrappers, applies the spec-level `decode_encode`
on `r.toRepr x`, then uses `from_to` to fold the round-tripped
representation `toRepr (fromRepr (toRepr x))` back through to `x` —
actually, more directly: `decode_encode` gives `deserialize r.shape
(serialize r.shape (toRepr x)) = .ok (toRepr x, _)`; our wrapper
then maps the `.ok` payload through `fromRepr`, giving
`.ok (fromRepr (toRepr x)) = .ok x` by `to_from`.

`SSZ.roundtrip` is gated by `BasicSupported r.shape` because that
is the subset on which the underlying `decode_encode` proof
currently lives; the gate loosens as the proof set widens. -/
theorem roundtrip {T : Type} [r : SSZRepr T]
    (h_sup : SSZType.BasicSupported r.shape) (x : T) :
    SSZ.deserialize (SSZ.serialize x) = .ok x := by
  unfold SSZ.deserialize SSZ.serialize
  rw [Proofs.decode_encode h_sup (r.toRepr x)]
  -- Goal: `(match .ok (toRepr x, _) with | .ok (y, _) => .ok (fromRepr y) | ...) = .ok x`.
  -- The `match` reduces because the scrutinee is a literal `.ok`;
  -- then `r.to_from` folds `fromRepr (toRepr x)` back to `x`.
  simp [r.to_from]

end SSZ

end SizzLean
