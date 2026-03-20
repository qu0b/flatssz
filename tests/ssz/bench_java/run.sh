#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
rm -rf out
mkdir -p out
javac -d out src/ssz/*.java src/flatbuffers_codegen/*.java src/Bench.java
java -cp out Bench
