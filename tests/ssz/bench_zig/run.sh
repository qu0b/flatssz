#!/bin/bash
set -e
cd "$(dirname "$0")"
ZIG=/tmp/zig-linux-x86_64-0.13.0/zig
$ZIG build -Doptimize=ReleaseFast && ./zig-out/bin/bench
