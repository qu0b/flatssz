//! SSZ runtime support for flatssz-generated Rust code.
//!
//! Provides [`Hasher`] for merkleization, [`HasherPool`] for reuse,
//! and [`SszError`] for decode errors.

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

// ---- Zero hashes ----

fn compute_zero_hashes() -> [[u8; 32]; 65] {
    let mut zh = [[0u8; 32]; 65];
    for i in 0..64 {
        let mut h = Sha256::new();
        h.update(zh[i]);
        h.update(zh[i]);
        zh[i + 1] = h.finalize().into();
    }
    zh
}

static ZERO_HASHES_INIT: std::sync::OnceLock<[[u8; 32]; 65]> = std::sync::OnceLock::new();

fn zero_hashes() -> &'static [[u8; 32]; 65] {
    ZERO_HASHES_INIT.get_or_init(compute_zero_hashes)
}

// ---- Hasher ----

/// Buffer-based SSZ merkleization engine.
pub struct Hasher {
    buf: Vec<u8>,
    tmp: [u8; 64],
}

impl Hasher {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(4096),
            tmp: [0u8; 64],
        }
    }

    pub fn reset(&mut self) {
        self.buf.clear();
    }

    // ---- Index / finish ----

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

    // ---- Merkleization ----

    pub fn merkleize(&mut self, idx: usize) {
        let input = self.buf[idx..].to_vec();
        let result = merkleize_impl(&input, 0);
        self.buf.truncate(idx);
        self.buf.extend_from_slice(&result);
    }

    pub fn merkleize_with_mixin(&mut self, idx: usize, num: u64, limit: u64) {
        self.fill_up_to_32();
        let input = self.buf[idx..].to_vec();
        let root = merkleize_impl(&input, limit);

        // Mix in length
        let mut combined = [0u8; 64];
        combined[..32].copy_from_slice(&root);
        combined[32..40].copy_from_slice(&num.to_le_bytes());
        // bytes 40..64 are already zero

        let hash = Sha256::digest(&combined);
        self.buf.truncate(idx);
        self.buf.extend_from_slice(&hash);
    }
}

fn get_depth(d: u64) -> u8 {
    if d <= 1 {
        return 0;
    }
    let i = d.next_power_of_two();
    63 - i.leading_zeros() as u8
}

fn merkleize_impl(input: &[u8], mut limit: u64) -> Vec<u8> {
    let count = ((input.len() + 31) / 32) as u64;
    if limit == 0 {
        limit = count;
    }
    let zh = zero_hashes();

    if limit == 0 {
        return zh[0].to_vec();
    }
    if limit == 1 {
        if count == 1 && input.len() >= 32 {
            return input[..32].to_vec();
        }
        return zh[0].to_vec();
    }

    let depth = get_depth(limit);
    if input.is_empty() {
        return zh[depth as usize].to_vec();
    }

    let mut data = input.to_vec();
    // Pad to 32-byte alignment
    let rest = data.len() % 32;
    if rest != 0 {
        data.extend_from_slice(&[0u8; 32][..32 - rest]);
    }

    for i in 0..depth {
        let layer_len = data.len() / 32;
        let odd = layer_len % 2 == 1;
        if odd {
            data.extend_from_slice(&zh[i as usize]);
        }
        let pairs = (data.len() / 32) / 2;
        let mut output = Vec::with_capacity(pairs * 32);
        for p in 0..pairs {
            let hash = Sha256::digest(&data[p * 64..(p + 1) * 64]);
            output.extend_from_slice(&hash);
        }
        data = output;
    }
    data
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
    // Trim trailing zeros
    while dst.last() == Some(&0) {
        dst.pop();
    }
    (dst, size)
}

// ---- HasherPool ----

/// Pool for reusing [`Hasher`] instances.
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
        let idx = 0;
        h.merkleize(idx);
        assert_eq!(h.buf.len(), 32);
    }

    #[test]
    fn test_roundtrip_basic() {
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
}
