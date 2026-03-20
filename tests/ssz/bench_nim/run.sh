#!/bin/bash
export PATH="$HOME/.nimble/bin:$PATH"
nim c -d:release --hints:off bench.nim && ./bench
