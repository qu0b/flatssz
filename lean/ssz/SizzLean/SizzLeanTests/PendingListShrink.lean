import SizzLean.Hasher.Sha256
import SizzLean.Repr.Class
import SizzLean.Repr.Instances
import SizzLean.Cache.TreeBacked
import SizzLean.Cache.Update
import SizzLeanTests.ExampleContainers

/-!
# `SizzLeanTests.PendingListShrink` — list-shrink + stale-index writes

A potential failure mode in the closure-based pending overlay:
write 1 references `xs[i]`; write 2 shortens `xs` so index `i`
no longer exists in the view. Does the commit still produce a
tree whose root matches `SSZ.hashTreeRoot view`?

Three cases:

| # | Sequence                                | Mechanism that keeps it right |
|---|-----------------------------------------|-------------------------------|
| 1 | `xs[i] := v` then `xs := shorter`       | Parent supersedes via commit-time drop (`commitAndHash` `[]`-path wins) |
| 2 | `xs := shorter` then `xs[i] := v` (`i` OOB in `shorter`) | View-side `set!` is OOB-no-op; parent still supersedes at commit |
| 3 | `xs[i] := v` directly with `i` OOB      | `PendingWrite T = T → Option Node` returns `none` when the view-side `set!` was an OOB no-op, so nothing is committed at position `i` and the cached root matches `SSZ.hashTreeRoot view` regardless of `default α`. |

All three (and the further OOB / mixed-clause cases below) produce
tree roots matching `SSZ.hashTreeRoot view`.

**Expected build output.** The OOB cases (4, 5, 7) exercise
`SSZList.set!` on the view side past the current length.
`Array.set!` prints `Error: index out of bounds` on the panic
path before returning the array unchanged — so `lake build
SizzLeanTests` surfaces a few

```
info: SizzLeanTests/PendingListShrink.lean:N:0: Error: index out of bounds
```

lines from native_decide evaluation. These are *expected*
runtime output from the deliberately-OOB inputs, not failures:
the `PendingWrite` closure detects the OOB via the bounds
guard, returns `none`, and the example closes. The
`Build completed successfully` status line is the authoritative
signal. The `just test-ssz` recipe prints a one-paragraph
heads-up before invoking the build.
-/

set_option autoImplicit false
set_option maxHeartbeats 400000

namespace SizzLeanTests.PendingListShrink

open SizzLean
open SizzLean.Hasher
open SizzLean.Cache
open SizzLean.Repr
open SizzLeanTests.ExampleContainers

private def mk (k : UInt8) : ExRoot := Vector.replicate 32 k

/-- Initial `vals` with 5 distinct entries. The cap is 8 (from
`ListShrinkExample.vals`), so we have 3 unused slots after the
populated five. -/
private def initialVals : SSZList ExRoot 8 :=
  ⟨#[mk 0x11, mk 0x22, mk 0x33, mk 0x44, mk 0x55], by decide⟩

/-- Same shape as `initialVals` but truncated to length 2 — only
positions 0 and 1 remain. Positions 2..4 disappear. -/
private def shorterVals : SSZList ExRoot 8 :=
  ⟨#[mk 0xa1, mk 0xa2], by decide⟩

private def s0 : ListShrinkExample :=
  { vals := initialVals, marker := 7 }

/-! ## Case 1 — index-then-shorten

`xs[3] := v`, then `xs := shorter` (length 2, no index 3 anymore).
Pending ends up `{gindex(vals), gindex(vals[3])}`. The view's
final `vals` is `shorter`. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t1 := sszUpdate t  with vals[3] := mk 0xff
    let t2 := sszUpdate t1 with vals    := shorterVals
    let expected : ListShrinkExample := { s0 with vals := shorterVals }
    t2.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 2 — shorten-then-index, index OOB in `shorter`

`xs := shorter` (length 2), then `xs[3] := v`. The view side runs
`shorter.set! 3 v`, which is an OOB no-op (Array.set! semantics
through SSZList.set!). View's `vals` ends as `shorter`. Pending
is `{gindex(vals), gindex(vals[3])}`. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t1 := sszUpdate t  with vals    := shorterVals
    let t2 := sszUpdate t1 with vals[3] := mk 0xff
    let expected : ListShrinkExample := { s0 with vals := shorterVals }
    t2.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 3 — bare OOB index, no parent write

`xs[3] := v` directly on a list of length 5, no whole-list
update. View's `vals` ends as the original (set! is in-bounds
here — only positions ≥ 5 would be OOB). Included as the
non-shrinking baseline. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t' := sszUpdate t with vals[3] := mk 0xff
    let expected : ListShrinkExample := { s0 with vals := initialVals.set! 3 (mk 0xff) }
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 4 — bare OOB index past current length (ExRoot baseline)

`xs[6] := v` on a list of length 5. View-side `initialVals.set! 6 v`
is an OOB no-op; view stays at length 5 with original values.
Cached path: pending `{gindex(vals[6])}`; closure projects
`view.vals[6]! = default ExRoot = Vector.replicate 32 0` (32 zero
bytes). Tree commits a zero leaf at position 6, which matches
the natural zero-padding for an SSZList shorter than its cap.

For `ExRoot` this is correct because `default ExRoot` has the
same merkle root as the zero-padding. See Case 5 for the
companion test on a non-zero-default element type. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t' := sszUpdate t with vals[6] := mk 0xff
    -- view.vals.set! 6 v is OOB no-op; expected view's vals = initialVals
    let expected : ListShrinkExample := s0
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 5 — bare OOB index, element type with non-zero `Inhabited`

`xs[6] := v` on a `SSZList NonZeroElem 8` of length 3, where
`default NonZeroElem = { a := 1, b := 1 }`. The view-side `set! 6 v`
is an OOB no-op so the view stays at length 3 with original values.

This case is the regression guard for the `PendingWrite T =
T → Option Node` design: under the earlier `T → Node` shape, the
closure for `vals[6]` projected `view.vals[6]! = default NonZeroElem
= { a := 1, b := 1 }`, built `Node.ofShape` of that non-zero
element, and wrote it into the tree at position 6 — diverging
from `SSZ.hashTreeRoot view`, which correctly zero-pads. The
current closure returns `none` when the view-side `set!` was an
OOB no-op, so the pending entry is dropped and the cached root
matches the spec root regardless of `default α`. -/
private def nzInitial : SSZList NonZeroElem 8 :=
  ⟨#[{ a := 10, b := 20 },
     { a := 11, b := 21 },
     { a := 12, b := 22 }], by decide⟩

private def nz0 : NonZeroListExample :=
  { vals := nzInitial, marker := 7 }

example :
    let t  : TreeBacked Sha256 NonZeroListExample := TreeBacked.ofValue Sha256 nz0
    let t' := sszUpdate t with vals[6] := { a := 99, b := 99 }
    let expected : NonZeroListExample := nz0
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 6 — same-statement whole-list write + index write

One `sszUpdate` statement with two clauses: `vals := shorter,
vals[1] := v`. The macro emits both into a single
`addPendingMany`. Pending ends up with both gindices; view's
let-chain applies them in sequence so `view.vals` is
`shorter.set! 1 v` (in-bounds — succeeds). The parent's
closure reads the final view at commit time and produces a
subtree that already includes the index update. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t' := sszUpdate t with
      vals    := shorterVals,
      vals[1] := mk 0xee
    -- view.vals = shorterVals.set! 1 (mk 0xee) — both writes apply
    let expected : ListShrinkExample :=
      { s0 with vals := shorterVals.set! 1 (mk 0xee) }
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 7 — same-statement whole-list write + OOB index write

`vals := shorter, vals[6] := v` in one statement. `shorter` has
length 2, so `set! 6 v` is a view-side OOB no-op. View's
`vals = shorter`. The closure for `vals[6]` returns `none`
(`6 ≥ shorter.size`); the closure for `vals` builds `shorter`'s
subtree. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t' := sszUpdate t with
      vals    := shorterVals,
      vals[6] := mk 0xff
    let expected : ListShrinkExample := { s0 with vals := shorterVals }
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

/-! ## Case 8 — same-statement length change exercised via reverse order

Macro emits clauses left-to-right but TreeMap order is gindex-
ascending. Reverse-order clauses (`vals[1] := v, vals := shorter`)
test that the macro's view-side let-chain still produces the
right final view (the second clause's whole-list write
overwrites the first clause's index change at the view level),
and the tree side mirrors that via parent supersession at
commit. -/
example :
    let t  : TreeBacked Sha256 ListShrinkExample := TreeBacked.ofValue Sha256 s0
    let t' := sszUpdate t with
      vals[1] := mk 0xcc,
      vals    := shorterVals
    -- view side: initial.set! 1 (mk 0xcc) then whole-replaced with shorter
    let expected : ListShrinkExample := { s0 with vals := shorterVals }
    t'.hashTreeRootCached.1 = SSZ.hashTreeRoot Sha256 expected := by
  native_decide

end SizzLeanTests.PendingListShrink
