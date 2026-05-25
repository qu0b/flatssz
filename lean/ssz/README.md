# Vendored SSZ runtime (SizzLean)

This directory vendors the Lean 4 SSZ runtime that backs the `--ssz-lean`
flatssz code generator:

- `SizzLean/` — SSZ serialize / deserialize / hash-tree-root with the
  `deriving SSZRepr` handler. The generated `*_ssz.lean` code consists of
  plain `structure … deriving SSZRepr` declarations; all wire behaviour
  comes from this library.
- `LeanSha256/` — pure-Lean SHA-256 reference used by SizzLean's tests.
- SHA-256 at runtime is SizzLean's OpenSSL `EVP` FFI shim
  (`SizzLean/csrc/sha256_shim.c`), linked against `libcrypto`.

## Provenance & license

Imported verbatim from **etheorem/etheorem** (`packages/SizzLean`,
`packages/LeanSha256`). That project is licensed under the **GNU LGPL**
(see `LICENSE` in this directory), which differs from flatssz's
Apache-2.0. The license is preserved here so this subtree remains under
its original terms; the rest of the repository is unaffected.

Toolchain: `leanprover/lean4:v4.29.1` (see `lean-toolchain`).

Build standalone with: `cd SizzLean && lake build SizzLean`.
