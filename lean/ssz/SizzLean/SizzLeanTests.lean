import SizzLeanTests.ReprExamples
import SizzLeanTests.SetAtRandom
import SizzLeanTests.Sha256Vectors
import SizzLeanTests.Sha256Equivalence
import SizzLeanTests.ExampleContainers
import SizzLeanTests.TreeBackedCoherence
import SizzLeanTests.TreeBackedSetField
import SizzLeanTests.MultiSetterIndex
import SizzLeanTests.PendingOverlayCoherence
import SizzLeanTests.PendingPrefixConflict
import SizzLeanTests.PendingListShrink
import SizzLeanTests.WidthsAndLists
import SizzLeanTests.Sha256BatchEquivalence
-- Hash-consing (Stage 17c) is deferred — its smart constructor is
-- not yet integrated into `Node.ofShape` / `setAt` /
-- `merkleRootWithCache`, so the user-facing `SSZ.FastBox` /
-- `TreeBacked` path doesn't benefit transparently. The
-- `HashConsCoherence` test gates the standalone primitive and is
-- kept on disk but not in the default test build until the
-- library-side integration lands.
import SizzLeanTests.SerializeCacheCoherence

/-!
# `SizzLeanTests` — SSZ-only empirical / property-test gates

Property-test gates that exercise the SSZ library *in isolation*
— no Eth consensus-spec types. Build with:

```
lake build SizzLeanTests
```

## What's here

* **SHA-256 conformance** — NIST CAVP `byte` & `long-byte` vectors
  (`Sha256Vectors.lean`), FFI-vs-spec hasher equivalence
  (`Sha256Equivalence.lean`).
* **Tree machinery** — `Node.setAt` and `Node.setManyAt` PRNG
  property tests (`SetAtRandom.lean`).
* **Cache machinery on example containers** — `TreeBacked`
  coherence (`hashTreeRootCached = SSZ.hashTreeRoot`),
  `sszUpdate` multi-field batched updates, vector-index `sszUpdate`.
  Containers used as test fixtures are defined locally in
  `ExampleContainers.lean` — small SSZ-shaped types analogous to
  Phase-0 `Fork` / `SignedBeaconBlockHeader` / `HistoricalBatch`
  but with no dependency on the LeanEthCS table.

Eth-driven conformance (real Phase-0 → Fulu containers,
`ssz_static` CLI dispatch) lives in `LeanEthCS`. The
two libraries share the same property-test patterns but operate on
different container surfaces.

## Why split

The default `lake build` doesn't rebuild these files; on iterative
work the heavy `native_decide` batches don't recompile until you
ask. Splitting Eth-using tests further (into LeanEthCS)
keeps the SSZ-library gates fast and dependency-light.
-/
