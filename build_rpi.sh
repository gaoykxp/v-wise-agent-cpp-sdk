#!/bin/bash
# Build script for Raspberry Pi (Ubuntu 24.04)

set -e

echo "=========================================="
echo "Building vobu for Raspberry Pi"
echo "=========================================="

# Check for required dependencies
echo ""
echo "Checking dependencies..."

MISSING_DEPS=""

if ! command -v cmake &> /dev/null; then
    MISSING_DEPS="$MISSING_DEPS cmake"
fi

if ! command -v g++ &> /dev/null; then
    MISSING_DEPS="$MISSING_DEPS g++"
fi

if [ ! -f /usr/include/curl/curl.h ] && [ ! -f /usr/local/include/curl/curl.h ]; then
    MISSING_DEPS="$MISSING_DEPS libcurl4-openssl-dev"
fi

if [ ! -f /usr/include/openssl/ssl.h ] && [ ! -f /usr/local/include/openssl/ssl.h ]; then
    MISSING_DEPS="$MISSING_DEPS libssl-dev"
fi

if [ ! -f /usr/include/spdlog/spdlog.h ] && [ ! -d third_party/spdlog/include ]; then
    MISSING_DEPS="$MISSING_DEPS libspdlog-dev"
fi

if [ ! -f /usr/include/nlohmann/json.hpp ] && [ ! -d third_party/nlohmann_json ]; then
    MISSING_DEPS="$MISSING_DEPS nlohmann-json3-dev"
fi

if [ -n "$MISSING_DEPS" ]; then
    echo ""
    echo "Missing dependencies detected:$MISSING_DEPS"
    echo ""
    echo "Please install them with:"
    echo "  sudo apt update"
    echo "  sudo apt install -y cmake g++ libcurl4-openssl-dev libssl-dev libspdlog-dev nlohmann-json3-dev"
    echo ""
    exit 1
fi

echo "All dependencies found!"

# Create build directory
BUILD_DIR="build"
mkdir -p ${BUILD_DIR}
cd ${BUILD_DIR}

echo ""
echo "Running CMake..."
cmake ..

echo ""
echo "Compiling..."
make -j$(nproc)

echo ""
echo "=========================================="
echo "Build complete!"
echo "=========================================="
echo ""
echo "Executable: ${BUILD_DIR}/vobu"
echo ""
echo "To run:"
echo "  1. Copy config file: sudo mkdir -p /conf && sudo cp ../data/config.json /conf/"
echo "  2. Run: ./${BUILD_DIR}/vobu"
echo ""