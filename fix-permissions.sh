#!/bin/bash
# Fix ownership and permissions for build artifacts
# Run this if files get owned by root

USER=$(whoami)

echo "Fixing ownership and permissions for $USER..."

# Fix editor build artifacts
if [ -d "editor/src-tauri/libs" ]; then
    sudo chown -R $USER:staff editor/src-tauri/libs
    chmod -R u+rw editor/src-tauri/libs
    echo "✓ Fixed editor/src-tauri/libs"
fi

if [ -d "editor/src-tauri/include" ]; then
    sudo chown -R $USER:staff editor/src-tauri/include
    chmod -R u+rw editor/src-tauri/include
    echo "✓ Fixed editor/src-tauri/include"
fi

if [ -d "editor/src-tauri/target" ]; then
    echo "  Fixing editor/src-tauri/target (this may take a moment)..."
    sudo chown -R $USER:staff editor/src-tauri/target
    chmod -R u+rw editor/src-tauri/target
    echo "✓ Fixed editor/src-tauri/target"
fi

# Fix CMake build directory
if [ -d "build" ]; then
    echo "  Fixing build/ directory..."
    sudo chown -R $USER:staff build
    chmod -R u+rw build
    echo "✓ Fixed build/"
fi

# Fix app data directory
APP_DATA_DIR="$HOME/Library/Application Support/com.tracey.editor"
if [ -d "$APP_DATA_DIR" ]; then
    echo "  Fixing app data directory..."
    sudo chown -R $USER:staff "$APP_DATA_DIR"
    chmod -R u+rw "$APP_DATA_DIR"
    echo "✓ Fixed $APP_DATA_DIR"
fi

echo ""
echo "✅ All permissions fixed!"
echo ""
echo "You can now run builds normally."
