import SizzLean.Repr.Class
import SizzLean.Repr.Instances
import SizzLean.Repr.Deriving
import SizzLean.Hasher.Sha256
import deneb_ssz

/-!
Cross-language SSZ benchmark — Lean backend (SizzLean runtime).

Loads the real Deneb mainnet `SignedBeaconBlock` (~130 KB), verifies the
hash-tree-root of the inner `BeaconBlock` against the known root, then times
unmarshal / marshal / hash-tree-root. Mirrors `tests/ssz/bench_nim/bench.nim`.

`SSZ.deserialize` / `SSZ.serialize` / `SSZ.hashTreeRoot` are SizzLean's
`deriving SSZRepr` API; SHA-256 is SizzLean's OpenSSL `Sha256` FFI hasher.
-/

open SizzLean
open SizzLean.Spec
open SizzLean.Hasher
open flatbuffers_codegen

/-- Lowercase-hex render of a byte array. -/
def toHex (b : ByteArray) : String :=
  let hexChar (n : Nat) : Char :=
    if n < 10 then Char.ofNat (n + '0'.toNat) else Char.ofNat (n - 10 + 'a'.toNat)
  b.toList.foldl
    (fun acc x => (acc.push (hexChar (x.toNat >>> 4))).push (hexChar (x.toNat &&& 0xf)))
    ""

def isHex (c : Char) : Bool :=
  c.isDigit || ('a' ≤ c && c ≤ 'f') || ('A' ≤ c && c ≤ 'F')

/-- Pull the 64-hex-char root out of `block-mainnet-meta.json` without a JSON
dependency: it's the only 64-char all-hex token between quotes. -/
def extractHtr (s : String) : Option String :=
  (s.splitOn "\"").find? (fun t => t.length == 64 && t.all isHex)

/-- Time `act` over `iters` iterations after `warmup`. `act` runs in `IO` and
reads its inputs from `IO.Ref`s, so the compiler cannot hoist the loop-invariant
pure work out of the loop — each iteration genuinely recomputes. The result is
folded into a sink and printed so it isn't dead-code eliminated. -/
def timeLoop (label : String) (warmup iters : Nat) (act : Unit → IO Nat) : IO Unit := do
  for _ in [0:warmup] do let _ ← act ()
  let t0 ← IO.monoNanosNow
  let mut sink := 0
  for _ in [0:iters] do sink := sink + (← act ())
  let t1 ← IO.monoNanosNow
  let ns := (t1 - t0) / iters
  IO.println s!"{label}: {ns} ns/op ({Float.ofNat ns / 1000.0} us) [sink={sink % 9973}]"

def main : IO Unit := do
  let data ← IO.FS.readBinFile "block-mainnet.ssz"
  let metaJson ← IO.FS.readFile "block-mainnet-meta.json"
  IO.println s!"SSZ data: {data.size} bytes"

  let blk ← match SSZ.deserialize (T := SignedBeaconBlock) data with
    | .ok b => pure b
    | .error _ => throw (IO.userError "SSZ deserialize failed")

  let root := SSZ.hashTreeRoot Sha256 blk.message
  let rootHex := toHex root
  match extractHtr metaJson with
  | some expected =>
    if rootHex == expected then IO.println s!"HTR ✓ {rootHex}"
    else
      IO.println s!"HTR MISMATCH\n  got {rootHex}\n  exp {expected}"
      throw (IO.userError "hash-tree-root mismatch")
  | none => IO.println "warning: no expected htr in meta"

  -- Read inputs through refs so the optimiser can't hoist the loop-invariant
  -- pure work out of the timing loops.
  let dataRef ← IO.mkRef data
  let blkRef ← IO.mkRef blk

  timeLoop "unmarshal" 100 1000 (fun _ => do
    let d ← dataRef.get
    match SSZ.deserialize (T := SignedBeaconBlock) d with
    | .ok b => pure b.message.slot.toNat
    | .error _ => pure 0)
  timeLoop "marshal" 100 5000 (fun _ => do
    let b ← blkRef.get
    pure (SSZ.serialize b).size)
  timeLoop "hash_tree_root" 20 200 (fun _ => do
    let b ← blkRef.get
    pure ((SSZ.hashTreeRoot Sha256 b.message).toList.foldl (fun a x => a + x.toNat) 0))
