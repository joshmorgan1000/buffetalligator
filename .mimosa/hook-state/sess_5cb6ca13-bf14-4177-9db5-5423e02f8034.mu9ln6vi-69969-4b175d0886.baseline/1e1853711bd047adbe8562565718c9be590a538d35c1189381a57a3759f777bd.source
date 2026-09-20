#!/bin/bash
set -e
echo "Building simple threadsafe logger..."
CURRENT_DIRECTORY=$(pwd)
SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIRECTORY"
rm -rf build
mkdir build
cd build
cmake .. -G Ninja
ninja
cd "$CURRENT_DIRECTORY"
echo "Running tests..."
# Make sure the return code for build/tests/test_logging is good
if [[ -x build/tests/test_logging ]]; then
    build/tests/test_logging
    if [[ $? -ne 0 ]]; then
        echo "Tests failed!"
        cd "$CURRENT_DIRECTORY"
        exit 1
    fi
else
    echo "Test executable not found!"
    cd "$CURRENT_DIRECTORY"
    exit 1
fi
# Also test build/tests/test_utilities
if [[ -x build/tests/test_utilities ]]; then
    build/tests/test_utilities
    if [[ $? -ne 0 ]]; then
        echo "Utilities tests failed!"
        cd "$CURRENT_DIRECTORY"
        exit 1
    fi
else
    echo "Utilities test executable not found!"
    cd "$CURRENT_DIRECTORY"
    exit 1
fi
echo "Build successful!"
