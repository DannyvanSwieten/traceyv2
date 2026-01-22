#!/bin/bash
set -e

# Ensure pnpm and node are in PATH
export PATH="$HOME/Library/pnpm:$PATH"

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Tracey Editor Production Build ==="
echo "Project root: $PROJECT_ROOT"
echo ""

# Build C++ library in release mode
echo "=== Building C++ library (Release) ==="
mkdir -p "$PROJECT_ROOT/build-release"
cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build-release" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_C_API=ON

cmake --build "$PROJECT_ROOT/build-release" --target tracey_c_api -j8

# Install headers and library to editor directory
cmake --install "$PROJECT_ROOT/build-release" 2>/dev/null || true

echo ""
echo "=== C++ build complete ==="
echo "Library installed to: $PROJECT_ROOT/editor/src-tauri/libs"
echo ""

# Build Tauri application
cd "$PROJECT_ROOT/editor"

# Check if npm/pnpm is available
if command -v pnpm &> /dev/null; then
    echo "=== Building Tauri application (using pnpm) ==="
    pnpm install
    pnpm tauri build
elif command -v npm &> /dev/null; then
    echo "=== Building Tauri application (using npm) ==="
    npm install
    npm run tauri build
else
    echo "Error: npm or pnpm not found"
    exit 1
fi

echo ""
echo "=== Build complete ==="
echo "Application bundle created in: $PROJECT_ROOT/editor/src-tauri/target/release/bundle"
