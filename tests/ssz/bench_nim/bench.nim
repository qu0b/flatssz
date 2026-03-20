import std/[times, os, json, strutils, monotimes]
import ssz_runtime
import deneb_ssz

proc hexToBytes(s: string): seq[byte] =
  result = newSeq[byte](s.len div 2)
  for i in 0 ..< result.len:
    result[i] = byte(parseHexInt(s[i*2 .. i*2+1]))

proc main() =
  let data = readFile("block-mainnet.ssz")
  let buf = cast[seq[byte]](data)
  let meta = parseJson(readFile("block-mainnet-meta.json"))
  let expectedHtr = hexToBytes(meta["htr"].getStr())
  
  echo "SSZ data: ", buf.len, " bytes"
  
  let blk = SignedBeaconBlock.fromSSZBytes(buf)
  let htr = blk.message.hashTreeRoot()
  
  var match = true
  for i in 0 ..< 32:
    if htr[i] != expectedHtr[i]:
      match = false
      break
  echo if match: "HTR ✓" else: "HTR MISMATCH"
  if not match: return

  var N: int
  var start: MonoTime
  var elapsed: Duration
  
  # Warmup
  for i in 0 ..< 100:
    discard SignedBeaconBlock.fromSSZBytes(buf)

  N = 1000
  start = getMonoTime()
  for i in 0 ..< N:
    discard SignedBeaconBlock.fromSSZBytes(buf)
  elapsed = getMonoTime() - start
  echo "unmarshal: ", elapsed.inMicroseconds div N, " us/op"

  N = 5000
  start = getMonoTime()
  for i in 0 ..< N:
    discard blk.marshalSSZ()
  elapsed = getMonoTime() - start
  echo "marshal: ", elapsed.inMicroseconds div N, " us/op"

  N = 200
  start = getMonoTime()
  for i in 0 ..< N:
    discard blk.message.hashTreeRoot()
  elapsed = getMonoTime() - start
  echo "hash_tree_root: ", elapsed.inMicroseconds div N, " us/op"

main()
