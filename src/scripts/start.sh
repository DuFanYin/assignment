#!/bin/bash

# Order Book Streamer - Standalone Version Startup Script
# Usage: ./start.sh [build|run]
#   build: Only build the project
#   run:   Only run the executables (default, assumes build already exists)

MODE=${1:-run}

if [[ "$MODE" != "build" && "$MODE" != "run" ]]; then
    echo "Usage: $0 [build|run]"
    echo "  build: Only build the project"
    echo "  run:   Only run the executables (default, assumes build already exists)"
    exit 1
fi

# Navigate to project root directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")")
cd "$SCRIPT_DIR/.."

# BUILD MODE: Only build the project
if [[ "$MODE" == "build" ]]; then
    echo "=========================================="
    echo "Order Book Streamer - Standalone Version"
    echo "=========================================="
    echo "Mode: build"
    echo ""
    echo "[1/3] Preparing build directory..."
    mkdir -p build
    cd build

    echo "[2/3] Configuring CMake..."
    cmake .. > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ CMake configuration failed"
        echo "Running CMake with verbose output:"
        cmake ..
        exit 1
    fi
    echo "✓ CMake configuration successful"

    echo "[3/3] Compiling project..."
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ Build failed"
        echo "Running make with verbose output:"
        make
        exit 1
    fi
    echo "✓ Build successful"

    echo ""
    echo "=========================================="
    echo "Build completed successfully"
    echo "Executables:"
    echo "  - build/apps/sender"
    echo "  - build/apps/receiver"
    echo "=========================================="
    exit 0
fi

# RUN MODE: Only execute the pipeline (assumes build exists)
# Navigate to build directory
cd build

# Check if executables exist
if [ ! -f "./apps/receiver" ] || [ ! -f "./apps/sender" ]; then
    echo "✗ Executables not found"
    echo "Please build the project first by running: ./scripts/start.sh build"
    exit 1
fi

# Clean up existing JSON output file if it exists
rm -f "../../data/order_book_output.json" 2>/dev/null

# Kill any existing processes on port 8080
lsof -ti:8080 | xargs kill -9 2>/dev/null
sleep 1

# Set config path
export ASSIGNMENT_CONFIG="../config/config.ini"

# Start sender in background first (it sets up the server)
./apps/sender &
SENDER_PID=$!

# Wait for sender to set up the server
sleep 1

# Check if sender is still running
if ! kill -0 $SENDER_PID 2>/dev/null; then
    echo "✗ Sender failed to start"
    exit 1
fi

# Start receiver (it connects to the sender) - program prints its own metrics
./apps/receiver
RECEIVER_EXIT=${PIPESTATUS[0]}

# Kill sender
kill $SENDER_PID 2>/dev/null
wait $SENDER_PID 2>/dev/null

# Check if output file was created
if [ -f "../../data/order_book_output.json" ]; then
    FILE_SIZE=$(du -h ../../data/order_book_output.json | cut -f1)
    RECORD_COUNT=$(wc -l < ../../data/order_book_output.json)
    echo ""
    echo "Output: ../../data/order_book_output.json ($FILE_SIZE, $RECORD_COUNT records)"
fi

exit $RECEIVER_EXIT
