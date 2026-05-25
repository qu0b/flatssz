import Lake
open Lake DSL

-- The Lean SSZ runtime (SizzLean) is vendored at the repo root under
-- `lean/ssz/`. Requiring it transitively pulls its `LeanSha256` dep and its
-- `libssz_sha256` extern_lib (the OpenSSL SHA-256 FFI shim).
require SizzLean from "../../../lean/ssz/SizzLean"

package «bench_lean» where
  -- SizzLean's SHA-256 shim calls OpenSSL `libcrypto`. A dependency's
  -- package-level `moreLinkArgs` do not propagate to a dependent exe's final
  -- link, so we repeat the libcrypto link here. Debian/Ubuntu multiarch path;
  -- matches SizzLean's own pkg-config fallback.
  moreLinkArgs := #["-L/usr/lib/x86_64-linux-gnu", "-l:libcrypto.so.3"]
  moreLeancArgs := #["-march=native"]

-- The flatc-generated types live in `deneb_ssz.lean`. Declare it as a lib so
-- Lake compiles it and the exe can `import deneb_ssz`.
lean_lib deneb_ssz where
  roots := #[`deneb_ssz]
  -- Load-bearing for the benchmark: without it the generated `SSZRepr`
  -- instances (shape, toRepr/fromRepr) run as interpreted bytecode, so the
  -- timings measure Lean's interpreter rather than compiled native code.
  precompileModules := true

@[default_target]
lean_exe bench where
  root := `Bench
  -- Root imports a `precompileModules` library (SizzLean); mirror SizzLean's
  -- own `ssz_bench` exe to avoid a Lake build-graph cycle.
  supportInterpreter := true
