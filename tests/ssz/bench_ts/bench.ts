import * as fs from 'fs';
import * as path from 'path';
import { SignedBeaconBlock } from './deneb_ssz';

const SSZ_PATH = '/home/framework/repos/ssz-benchmark/res/block-mainnet.ssz';
const META_PATH = '/home/framework/repos/ssz-benchmark/res/block-mainnet-meta.json';

const sszData = new Uint8Array(fs.readFileSync(SSZ_PATH));
const meta = JSON.parse(fs.readFileSync(META_PATH, 'utf-8'));
const expectedHtr = meta.htr as string;

function toHex(bytes: Uint8Array): string {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

// Warmup: unmarshal once to verify correctness and pre-populate any lazy state
const block = SignedBeaconBlock.fromSszBytes(sszData);

// Verify round-trip marshal
const remarshaled = block.toSszBytes();
if (remarshaled.length !== sszData.length) {
  throw new Error(
    `marshal length mismatch: ${remarshaled.length} vs ${sszData.length}`
  );
}
for (let i = 0; i < sszData.length; i++) {
  if (remarshaled[i] !== sszData[i]) {
    throw new Error(`marshal mismatch at byte ${i}`);
  }
}
console.log('round-trip marshal: OK');

// Verify HTR (meta.json contains the BeaconBlock HTR, not SignedBeaconBlock)
const htr = block.message.treeHashRoot();
const htrHex = toHex(htr);
if (htrHex !== expectedHtr) {
  throw new Error(`HTR mismatch: got ${htrHex}, expected ${expectedHtr}`);
}
console.log('HTR verification: OK');

// --- Benchmarks ---

function benchUnmarshal(iterations: number): number {
  const start = performance.now();
  for (let i = 0; i < iterations; i++) {
    SignedBeaconBlock.fromSszBytes(sszData);
  }
  const elapsed = performance.now() - start;
  return (elapsed * 1000) / iterations; // us/op
}

function benchMarshal(iterations: number): number {
  // Pre-unmarshal the block
  const b = SignedBeaconBlock.fromSszBytes(sszData);
  const start = performance.now();
  for (let i = 0; i < iterations; i++) {
    b.toSszBytes();
  }
  const elapsed = performance.now() - start;
  return (elapsed * 1000) / iterations; // us/op
}

function benchHashTreeRoot(iterations: number): number {
  const b = SignedBeaconBlock.fromSszBytes(sszData);
  const start = performance.now();
  for (let i = 0; i < iterations; i++) {
    b.treeHashRoot();
  }
  const elapsed = performance.now() - start;
  return (elapsed * 1000) / iterations; // us/op
}

// Calibrate: run enough iterations to take ~2s per benchmark
function calibrate(fn: (n: number) => number, targetMs: number = 2000): number {
  // Start with 1 iteration to estimate time
  const probe = fn(1);
  const probeMs = probe / 1000; // convert us to ms
  if (probeMs <= 0) return 100;
  const n = Math.max(1, Math.floor(targetMs / probeMs));
  return n;
}

console.log('\ncalibrating...');

const nUnmarshal = calibrate((n) => benchUnmarshal(n));
const nMarshal = calibrate((n) => benchMarshal(n));
const nHtr = calibrate((n) => benchHashTreeRoot(n));

console.log(
  `iterations: unmarshal=${nUnmarshal}, marshal=${nMarshal}, htr=${nHtr}\n`
);

const unmarshalUs = benchUnmarshal(nUnmarshal);
console.log(`unmarshal: ${unmarshalUs.toFixed(0)} us/op`);

const marshalUs = benchMarshal(nMarshal);
console.log(`marshal: ${marshalUs.toFixed(0)} us/op`);

const htrUs = benchHashTreeRoot(nHtr);
console.log(`hash_tree_root: ${htrUs.toFixed(0)} us/op`);
