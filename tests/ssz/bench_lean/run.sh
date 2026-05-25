#!/bin/bash
set -e
cd "$(dirname "$0")"
lake build bench && ./.lake/build/bin/bench
