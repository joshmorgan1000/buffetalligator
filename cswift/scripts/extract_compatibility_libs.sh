#!/bin/bash

# Extract Swift compatibility libraries script
# Usage: extract_compatibility_libs.sh <target_dir> <swift_runtime_dir>
# This script is called automatically by CMake during the build process

set -e

TARGET_DIR="$1"
SWIFT_RUNTIME_DIR="$2"

if [ -z "$TARGET_DIR" ] || [ -z "$SWIFT_RUNTIME_DIR" ]; then
    echo "Usage: $0 <target_dir> <swift_runtime_dir>"
    exit 1
fi

cd "$TARGET_DIR"

echo "Extracting Swift compatibility libraries from: $SWIFT_RUNTIME_DIR"

# Extract Swift compatibility libraries
for lib in libswiftCompatibility56.a libswiftCompatibilityConcurrency.a libswiftCompatibilityPacks.a; do
    if [ -f "${SWIFT_RUNTIME_DIR}/${lib}" ]; then
        echo "  Processing ${lib}..."
        
        # For cross-platform compatibility, try to extract the right architecture
        if command -v lipo >/dev/null 2>&1; then
            # On macOS, extract the current architecture
            arch=$(uname -m)
            if [ "$arch" = "arm64" ]; then
                lipo "${SWIFT_RUNTIME_DIR}/${lib}" -thin arm64 -output "${lib}.arch" 2>/dev/null || cp "${SWIFT_RUNTIME_DIR}/${lib}" "${lib}.arch"
            else
                lipo "${SWIFT_RUNTIME_DIR}/${lib}" -thin x86_64 -output "${lib}.arch" 2>/dev/null || cp "${SWIFT_RUNTIME_DIR}/${lib}" "${lib}.arch"
            fi
            ar -x "${lib}.arch"
            rm -f "${lib}.arch"
        else
            # On Linux or other platforms, just extract directly
            ar -x "${SWIFT_RUNTIME_DIR}/${lib}" 2>/dev/null || true
        fi
    else
        echo "  Warning: ${lib} not found in ${SWIFT_RUNTIME_DIR}"
    fi
done

# Also include additional Swift runtime support libraries if available
for lib in libswiftCompatibility50.a libswiftCompatibility51.a libswiftCompatibilityDynamicReplacements.a; do
    if [ -f "${SWIFT_RUNTIME_DIR}/${lib}" ]; then
        echo "  Processing ${lib}..."
        
        if command -v lipo >/dev/null 2>&1; then
            arch=$(uname -m)
            if [ "$arch" = "arm64" ]; then
                lipo "${SWIFT_RUNTIME_DIR}/${lib}" -thin arm64 -output "${lib}.arch" 2>/dev/null || cp "${SWIFT_RUNTIME_DIR}/${lib}" "${lib}.arch"
            else
                lipo "${SWIFT_RUNTIME_DIR}/${lib}" -thin x86_64 -output "${lib}.arch" 2>/dev/null || cp "${SWIFT_RUNTIME_DIR}/${lib}" "${lib}.arch"
            fi
            ar -x "${lib}.arch" 2>/dev/null || true
            rm -f "${lib}.arch"
        else
            ar -x "${SWIFT_RUNTIME_DIR}/${lib}" 2>/dev/null || true
        fi
    fi
done

echo "Extracted $(ls -1 *.o 2>/dev/null | wc -l) object files total"