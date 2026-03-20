#!/bin/bash
set -e
cd "$(dirname "$0")"
export PATH="$HOME/.dotnet:$PATH"
export LD_LIBRARY_PATH=/home/framework/repos/flatbuffers/lib:${LD_LIBRARY_PATH:-}
dotnet run -c Release
