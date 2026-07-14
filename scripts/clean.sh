#!/bin/bash
# Clean build directories
set -e

cd "$(dirname "$0")/.."

echo "=== Cleaning build directories ==="
rm -rf build build_debug

echo "✅ Clean completed!"
