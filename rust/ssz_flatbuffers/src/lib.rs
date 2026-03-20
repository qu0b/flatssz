//! SSZ runtime support for flatssz-generated Rust code.
//!
//! Provides [`Hasher`] for merkleization, [`HasherPool`] for reuse,
//! and [`SszError`] for decode errors.
//!
//! Merkleization uses `hashtree-rs` for batch SIMD SHA-256 (AVX2/AVX-512/SHA-NI)
//! — the same C library used by Go's dynamic-ssz via prysmaticlabs/hashtree.
//! Falls back to `sha2` crate for single hashes (mixin).

use sha2::{Digest, Sha256};
use std::sync::Mutex;

// ---- Errors ----

#[derive(Debug, Clone, PartialEq)]
pub enum SszError {
    BufferTooSmall,
    InvalidOffset,
    InvalidBool,
}

impl std::fmt::Display for SszError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SszError::BufferTooSmall => write!(f, "ssz: buffer too small"),
            SszError::InvalidOffset => write!(f, "ssz: invalid offset"),
            SszError::InvalidBool => write!(f, "ssz: invalid bool value"),
        }
    }
}

impl std::error::Error for SszError {}

// ---- hashtree init ----

static HASHTREE_INIT: std::sync::Once = std::sync::Once::new();

fn ensure_hashtree_init() {
    HASHTREE_INIT.call_once(|| {
        hashtree_rs::init();
    });
}

// ---- Zero hashes ----

fn compute_zero_hashes() -> [[u8; 32]; 65] {
    ensure_hashtree_init();
    let mut zh = [[0u8; 32]; 65];
    let mut pair = [0u8; 64];
    let mut out = [0u8; 32];
    for i in 0..64 {
        pair[..32].copy_from_slice(&zh[i]);
        pair[32..].copy_from_slice(&zh[i]);
        hashtree_rs::hash(&mut out, &pair, 1);
        zh[i + 1] = out;
    }
    zh
}

static ZERO_HASHES_INIT: std::sync::OnceLock<[[u8; 32]; 65]> = std::sync::OnceLock::new();

fn zero_hashes() -> &'static [[u8; 32]; 65] {
    ZERO_HASHES_INIT.get_or_init(compute_zero_hashes)
}

// ---- Hasher ----

/// Buffer-based SSZ merkleization engine.
///
/// Uses `hashtree-rs` for batch SIMD hashing of merkle tree layers
/// and in-place buffer operations for zero intermediate allocations.
pub struct Hasher {
    buf: Vec<u8>,
    out: Vec<u8>,
    tmp: [u8; 64],
}

impl Hasher {
    pub fn new() -> Self {
        ensure_hashtree_init();
        Self {
            buf: Vec::with_capacity(8192),
            out: Vec::with_capacity(4096),
            tmp: [0u8; 64],
        }
    }

    pub fn reset(&mut self) {
        self.buf.clear();
        self.out.clear();
    }

    pub fn index(&self) -> usize {
        self.buf.len()
    }

    pub fn finish(&self) -> [u8; 32] {
        let mut out = [0u8; 32];
        if self.buf.len() >= 32 {
            out.copy_from_slice(&self.buf[self.buf.len() - 32..]);
        }
        out
    }

    // ---- Put methods (32-byte padded chunks) ----

    pub fn append_bytes32(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
        let rest = b.len() % 32;
        if rest != 0 {
            self.buf.extend_from_slice(&[0u8; 32][..32 - rest]);
        }
    }

    pub fn put_u64(&mut self, v: u64) {
        self.tmp[..8].copy_from_slice(&v.to_le_bytes());
        self.tmp[8..32].fill(0);
        self.buf.extend_from_slice(&self.tmp[..32]);
    }

    pub fn put_u32(&mut self, v: u32) {
        self.tmp[..4].copy_from_slice(&v.to_le_bytes());
        self.tmp[4..32].fill(0);
        self.buf.extend_from_slice(&self.tmp[..32]);
    }

    pub fn put_u16(&mut self, v: u16) {
        self.tmp[..2].copy_from_slice(&v.to_le_bytes());
        self.tmp[2..32].fill(0);
        self.buf.extend_from_slice(&self.tmp[..32]);
    }

    pub fn put_u8(&mut self, v: u8) {
        self.tmp[0] = v;
        self.tmp[1..32].fill(0);
        self.buf.extend_from_slice(&self.tmp[..32]);
    }

    pub fn put_bool(&mut self, v: bool) {
        self.tmp[..32].fill(0);
        if v {
            self.tmp[0] = 1;
        }
        self.buf.extend_from_slice(&self.tmp[..32]);
    }

    pub fn put_bytes(&mut self, b: &[u8]) {
        if b.len() <= 32 {
            self.append_bytes32(b);
            return;
        }
        let idx = self.index();
        self.append_bytes32(b);
        self.merkleize(idx);
    }

    pub fn put_bitlist(&mut self, bb: &[u8], max_size: u64) {
        let (bitlist, size) = parse_bitlist(bb);
        let idx = self.index();
        self.append_bytes32(&bitlist);
        self.merkleize_with_mixin(idx, size, (max_size + 255) / 256);
    }

    // ---- Append methods (no padding) ----

    pub fn append_bool(&mut self, v: bool) {
        self.buf.push(if v { 1 } else { 0 });
    }

    pub fn append_u8(&mut self, v: u8) {
        self.buf.push(v);
    }

    pub fn append_u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn append_u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn append_u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn fill_up_to_32(&mut self) {
        let rest = self.buf.len() % 32;
        if rest != 0 {
            self.buf.extend_from_slice(&[0u8; 32][..32 - rest]);
        }
    }

    // ---- Merkleization (in-place, batch SIMD) ----

    pub fn merkleize(&mut self, idx: usize) {
        self.merkleize_inner(idx, 0);
    }

    pub fn merkleize_with_mixin(&mut self, idx: usize, num: u64, limit: u64) {
        self.fill_up_to_32();
        self.merkleize_inner(idx, limit);

        // Mix in length: hash(root || encode(num))
        debug_assert!(self.buf.len() == idx + 32);
        self.tmp[..32].copy_from_slice(&self.buf[idx..idx + 32]);
        self.tmp[32..40].copy_from_slice(&num.to_le_bytes());
        self.tmp[40..64].fill(0);

        // Single hash for mixin — use sha2 (one call, no batch needed)
        let hash: [u8; 32] = Sha256::digest(&self.tmp).into();
        self.buf[idx..idx + 32].copy_from_slice(&hash);
    }

    pub fn put_zero_hash(&mut self) {
        self.buf.extend_from_slice(&[0u8; 32]);
    }

    pub fn merkleize_progressive(&mut self, idx: usize) {
        self.fill_up_to_32();
        let input_len = self.buf.len() - idx;
        let count = (input_len / 32) as usize;
        let zh = zero_hashes();

        if count == 0 {
            self.buf.truncate(idx);
            self.buf.extend_from_slice(&zh[0]);
            return;
        }
        if count == 1 {
            // Single chunk: already a 32-byte root
            self.buf.truncate(idx + 32);
            return;
        }

        // EIP-7916 progressive subtree merkleization.
        // Subtree sizes: 1, 4, 16, 64, 256, ... (1, 4^1, 4^2, 4^3, ...)
        // For each subtree: extract chunks, merkleize as binary tree (pad with
        // zero hashes), produce a 32-byte root.
        // Chain: hash(subtree_1_root, hash(subtree_2_root, hash(..., zero_hash)))

        let mut subtree_sizes = Vec::new();
        {
            let mut remaining = count;
            let first = std::cmp::min(1, remaining);
            if first > 0 {
                subtree_sizes.push(first);
                remaining -= first;
            }
            let mut sz = 4usize;
            while remaining > 0 {
                let take = std::cmp::min(sz, remaining);
                subtree_sizes.push(take);
                remaining -= take;
                sz = sz.saturating_mul(4);
            }
        }

        // Compute root for each subtree via merkleize_impl
        let mut subtree_roots = Vec::with_capacity(subtree_sizes.len());
        let mut chunk_offset = idx;
        let mut expected_cap = 1usize;
        for &sz in &subtree_sizes {
            // Use a temporary buffer for binary merkleization
            let mut tmp_hasher = Hasher::new();
            tmp_hasher.buf.extend_from_slice(
                &self.buf[chunk_offset..chunk_offset + sz * 32],
            );
            tmp_hasher.merkleize_inner(0, expected_cap as u64);
            let mut root = [0u8; 32];
            root.copy_from_slice(&tmp_hasher.buf[..32]);
            subtree_roots.push(root);
            chunk_offset += sz * 32;
            expected_cap = expected_cap.saturating_mul(4);
        }

        // Right-fold: hash(root_0, hash(root_1, hash(root_2, ... hash(root_n-1, zero_hash))))
        let mut acc = zh[0];
        for root in subtree_roots.into_iter().rev() {
            self.tmp[..32].copy_from_slice(&root);
            self.tmp[32..64].copy_from_slice(&acc);
            let hash: [u8; 32] = Sha256::digest(&self.tmp).into();
            acc = hash;
        }

        self.buf.truncate(idx);
        self.buf.extend_from_slice(&acc);
    }

    pub fn merkleize_progressive_with_mixin(&mut self, idx: usize, num: u64) {
        self.merkleize_progressive(idx);

        // Mix in length: hash(root || encode(num))
        debug_assert!(self.buf.len() == idx + 32);
        self.tmp[..32].copy_from_slice(&self.buf[idx..idx + 32]);
        self.tmp[32..40].copy_from_slice(&num.to_le_bytes());
        self.tmp[40..64].fill(0);
        let hash: [u8; 32] = Sha256::digest(&self.tmp).into();
        self.buf[idx..idx + 32].copy_from_slice(&hash);
    }

    pub fn merkleize_progressive_with_active_fields(
        &mut self,
        idx: usize,
        active_fields: &[u8],
    ) {
        self.merkleize_progressive(idx);

        // Mix in active_fields bitvector: hash(root || pack_bits(active_fields))
        debug_assert!(self.buf.len() == idx + 32);
        self.tmp[..32].copy_from_slice(&self.buf[idx..idx + 32]);
        self.tmp[32..64].fill(0);
        let copy_len = std::cmp::min(active_fields.len(), 32);
        self.tmp[32..32 + copy_len].copy_from_slice(&active_fields[..copy_len]);
        let hash: [u8; 32] = Sha256::digest(&self.tmp).into();
        self.buf[idx..idx + 32].copy_from_slice(&hash);
    }

    fn merkleize_inner(&mut self, idx: usize, mut limit: u64) {
        let input_len = self.buf.len() - idx;
        let count = ((input_len + 31) / 32) as u64;
        if limit == 0 {
            limit = count;
        }
        let zh = zero_hashes();

        if limit == 0 {
            self.buf.truncate(idx);
            self.buf.extend_from_slice(&zh[0]);
            return;
        }
        if limit == 1 {
            if count >= 1 && input_len >= 32 {
                self.buf.truncate(idx + 32);
            } else {
                self.buf.truncate(idx);
                self.buf.extend_from_slice(&zh[0]);
            }
            return;
        }

        let depth = get_depth(limit);
        if input_len == 0 {
            self.buf.truncate(idx);
            self.buf.extend_from_slice(&zh[depth as usize]);
            return;
        }

        // Pad to 32-byte alignment
        let rest = (self.buf.len() - idx) % 32;
        if rest != 0 {
            self.buf.extend_from_slice(&[0u8; 32][..32 - rest]);
        }

        // In-place layer-by-layer hashing using batch SIMD
        for i in 0..depth {
            let layer_len = (self.buf.len() - idx) / 32;

            if layer_len % 2 == 1 {
                self.buf.extend_from_slice(&zh[i as usize]);
            }

            let pairs = (self.buf.len() - idx) / 64;

            // Batch hash ALL pairs in this layer with one SIMD call.
            // hashtree_rs::hash(out, chunks, count) hashes `count` pairs
            // of 32-byte chunks using AVX2/AVX-512/SHA-NI.
            self.out.resize(pairs * 32, 0);
            hashtree_rs::hash(
                &mut self.out[..pairs * 32],
                &self.buf[idx..idx + pairs * 64],
                pairs,
            );
            self.buf.truncate(idx);
            self.buf.extend_from_slice(&self.out[..pairs * 32]);
        }
    }
}

fn get_depth(d: u64) -> u8 {
    if d <= 1 {
        return 0;
    }
    let i = d.next_power_of_two();
    63 - i.leading_zeros() as u8
}

fn parse_bitlist(buf: &[u8]) -> (Vec<u8>, u64) {
    if buf.is_empty() {
        return (Vec::new(), 0);
    }
    let last = buf[buf.len() - 1];
    if last == 0 {
        return (Vec::new(), 0);
    }
    let msb = 7 - last.leading_zeros() as u8;
    let size = 8 * (buf.len() as u64 - 1) + msb as u64;
    let mut dst = buf.to_vec();
    let last_idx = dst.len() - 1;
    dst[last_idx] &= !(1u8 << msb);
    while dst.last() == Some(&0) {
        dst.pop();
    }
    (dst, size)
}

// ---- HasherPool ----

pub struct HasherPool;

static POOL: Mutex<Vec<Hasher>> = Mutex::new(Vec::new());

impl HasherPool {
    pub fn get() -> Hasher {
        POOL.lock()
            .ok()
            .and_then(|mut pool| pool.pop())
            .unwrap_or_else(Hasher::new)
    }

    pub fn put(mut h: Hasher) {
        h.reset();
        if let Ok(mut pool) = POOL.lock() {
            if pool.len() < 16 {
                pool.push(h);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_merkleize_single_chunk() {
        let mut h = Hasher::new();
        h.put_u64(42);
        h.merkleize(0);
        assert_eq!(h.buf.len(), 32);
    }

    #[test]
    fn test_deterministic() {
        let mut h = Hasher::new();
        let idx = h.index();
        h.put_u64(1);
        h.put_u64(2);
        h.merkleize(idx);
        let root1 = h.finish();

        h.reset();
        let idx = h.index();
        h.put_u64(1);
        h.put_u64(2);
        h.merkleize(idx);
        let root2 = h.finish();

        assert_eq!(root1, root2);
    }

    #[test]
    fn test_mixin() {
        let mut h = Hasher::new();
        let idx = h.index();
        h.append_bytes32(&[1u8; 4]);
        h.merkleize_with_mixin(idx, 4, 32);
        let root = h.finish();
        assert_ne!(root, [0u8; 32]);
    }
}
