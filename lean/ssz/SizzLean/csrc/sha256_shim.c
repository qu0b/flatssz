// SizzLean — C shim wrapping OpenSSL's SHA-256.
//
// Exposes two symbols that the Lean side declares as `@[extern]`:
//   * lean_ssz_sha256_hash    — single-input digest.
//   * lean_ssz_sha256_combine — two-input concatenation digest
//     (used for the inner Merkle step; pulled out as its own
//     primitive so SHA-NI / AVX-512 implementations can dispatch
//     to a two-block API later — see ARCHITECTURE.md §9.1).
//
// Both return a freshly-allocated 32-byte `ByteArray`. Inputs are
// borrowed (`b_lean_obj_arg` = `@&` on the Lean side); we do not
// touch their refcounts.
//
// Day-1 backend is OpenSSL's 3.x `EVP_*` API — the legacy `SHA256_*`
// functions are deprecated and emit compile-time warnings on
// recent platforms. The EVP form is the same call sequence everywhere
// OpenSSL ships and is the supported migration target.
//
// Trust assumption: the linked OpenSSL implements NIST FIPS 180-4
// SHA-256 correctly. Validated empirically by
// `SizzLean/Conformance/Sha256Vectors.lean` (Stage 9d).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <openssl/evp.h>
#include <lean/lean.h>

// glibc 2.34+ removed `__libc_csu_init` / `__libc_csu_fini`. Lean's
// bundled `Scrt1.o` (used when linking `lake exe` binaries) still
// references them, so on modern Debian/Ubuntu the final link fails
// with `undefined symbol: __libc_csu_init`. Stub them out here so
// the executable links. The stubs are never called at runtime (newer
// glibc's `_start` doesn't invoke them); they exist purely to
// satisfy the linker's symbol-resolution pass.
//
// This is a known Lean-on-glibc-2.34+ issue; the cleaner long-term
// fix is to rebuild Lean's startup files against current glibc.
void __libc_csu_init(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
}
void __libc_csu_fini(void) {}

// SHA-256 digest size in bytes. Hardcoded because the spec is
// SHA-256 (not parameterised over hash family); the Stage 17
// `Sha256Spec` hardening will swap the implementation but the
// digest width remains 32.
#define LEAN_SSZ_SHA256_DIGEST_LEN 32

// Internal helper: digest `len` bytes at `data` into `out` (32 bytes).
// Returns 1 on success, 0 on failure. Failure is treated as a fatal
// error and Lean is told via panic — there's no recovery story for
// an OpenSSL EVP failure inside an SSZ Merkle-root computation.
static int lean_ssz_sha256_raw(
    const uint8_t *data, size_t len, uint8_t out[LEAN_SSZ_SHA256_DIGEST_LEN])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)
          && EVP_DigestUpdate(ctx, data, len)
          && EVP_DigestFinal_ex(ctx, out, NULL);
    EVP_MD_CTX_free(ctx);
    return ok;
}

// SHA-256 of an arbitrary-length input. Exposed to Lean via
// `@[extern "lean_ssz_sha256_hash"] opaque sha256Hash`.
LEAN_EXPORT lean_obj_res lean_ssz_sha256_hash(b_lean_obj_arg input) {
    const size_t n = lean_sarray_size(input);
    const uint8_t *bytes = (const uint8_t *)lean_sarray_cptr(input);

    lean_obj_res out = lean_alloc_sarray(
        /* elem_size */ 1,
        /* size */ LEAN_SSZ_SHA256_DIGEST_LEN,
        /* capacity */ LEAN_SSZ_SHA256_DIGEST_LEN);

    if (!lean_ssz_sha256_raw(bytes, n, (uint8_t *)lean_sarray_cptr(out))) {
        lean_internal_panic("SizzLean: SHA-256 digest failed (OpenSSL EVP error)");
    }
    return out;
}

// SHA-256 of `left ++ right` (concatenation), without materialising
// the concatenation. Used for the inner Merkle step; both inputs
// are typically 32 bytes but we don't enforce that — callers that
// need the invariant should check it themselves.
LEAN_EXPORT lean_obj_res lean_ssz_sha256_combine(
    b_lean_obj_arg left, b_lean_obj_arg right)
{
    const size_t ln = lean_sarray_size(left);
    const size_t rn = lean_sarray_size(right);
    const uint8_t *lp = (const uint8_t *)lean_sarray_cptr(left);
    const uint8_t *rp = (const uint8_t *)lean_sarray_cptr(right);

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        lean_internal_panic("SizzLean: EVP_MD_CTX_new failed");
    }

    lean_obj_res out = lean_alloc_sarray(
        1, LEAN_SSZ_SHA256_DIGEST_LEN, LEAN_SSZ_SHA256_DIGEST_LEN);

    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)
          && EVP_DigestUpdate(ctx, lp, ln)
          && EVP_DigestUpdate(ctx, rp, rn)
          && EVP_DigestFinal_ex(ctx, (uint8_t *)lean_sarray_cptr(out), NULL);

    EVP_MD_CTX_free(ctx);

    if (!ok) {
        lean_internal_panic("SizzLean: SHA-256 combine failed (OpenSSL EVP error)");
    }
    return out;
}
