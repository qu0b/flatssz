#!/bin/bash
set -e
cd "$(dirname "$0")"
export PATH="$HOME/.nimble/bin:$PATH"
nim c -d:release --hints:off --path:. bench.nim && ./bench
