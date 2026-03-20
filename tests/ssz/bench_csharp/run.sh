#!/bin/bash
set -e
cd "$(dirname "$0")"
export PATH="$HOME/.dotnet:$PATH"
dotnet run -c Release
