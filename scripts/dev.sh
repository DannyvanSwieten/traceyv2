#!/bin/bash
set -e

# Ensure pnpm and node are in PATH
export PATH="$HOME/Library/pnpm:$PATH"

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Tracey Editor Development Mode ==="
echo "Project root: $PROJECT_ROOT"
echo ""

# Build C++ library in debug mode
echo "=== Building C++ library (Debug) ==="
mkdir -p "$PROJECT_ROOT/build"
cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_C_API=ON

cmake --build "$PROJECT_ROOT/build" --target tracey_c_api -j8

# Install headers and library to editor directory
cmake --install "$PROJECT_ROOT/build" 2>/dev/null || true

echo ""
echo "=== C++ build complete ==="
echo "Library installed to: $PROJECT_ROOT/editor/src-tauri/libs"
echo ""

# Run Tauri dev mode (this will build Rust and start the frontend)
cd "$PROJECT_ROOT/editor"

# Check if npm/pnpm is available
if command -v pnpm &> /dev/null; then
    echo "=== Starting Tauri dev server (using pnpm) ==="
    pnpm install
    pnpm tauri dev
elif command -v npm &> /dev/null; then
    echo "=== Starting Tauri dev server (using npm) ==="
    npm install
    npm run tauri dev
else
    echo "Error: npm or pnpm not found"
    exit 1
fi
