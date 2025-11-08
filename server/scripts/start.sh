#!/bin/bash

# WebSocket Server - Build and Run Script
# Usage: ./start.sh [build|run]
#   build: Build databento-cpp, uWebSockets/uSockets, and the project
#   run:   Run the WebSocket server (default, assumes build already exists)

MODE=${1:-run}

if [[ "$MODE" != "build" && "$MODE" != "run" ]]; then
    echo "Usage: $0 [build|run]"
    echo "  build: Build databento-cpp, uWebSockets/uSockets, and the project"
    echo "  run:   Run the WebSocket server (default, assumes build already exists)"
    exit 1
fi

# Navigate to project root directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")")
cd "$SCRIPT_DIR/.."

# BUILD MODE: Build third-party libraries and the project
if [[ "$MODE" == "build" ]]; then
    echo "=========================================="
    echo "WebSocket Server - Build"
    echo "=========================================="
    echo ""
    
    # Step 1: Clone and build databento-cpp
    echo "[1/5] Setting up databento-cpp..."
    cd thirdparty
    
    if [ ! -d "databento-cpp" ]; then
        echo "  Cloning databento-cpp..."
        git clone https://github.com/databento/databento-cpp.git > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ Failed to clone databento-cpp"
            exit 1
        fi
    fi
    
    cd databento-cpp
    
    # Build databento-cpp if not already built
    if [ ! -d "build" ] || [ ! -f "build/lib/Debug/libdatabento.a" ]; then
        echo "  Building databento-cpp..."
        mkdir -p build
        cd build
        cmake .. > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ databento-cpp CMake configuration failed"
            echo "Running CMake with verbose output:"
            cmake ..
            exit 1
        fi
        make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ databento-cpp build failed"
            echo "Running make with verbose output:"
            make
            exit 1
        fi
        echo "✓ databento-cpp built successfully"
    else
        echo "✓ databento-cpp already built"
    fi
    
    cd "$SCRIPT_DIR/.."
    
    # Step 2: Build uSockets (required by uWebSockets)
    echo "[2/5] Building uSockets..."
    cd thirdparty/uWebSockets
    
    # Check if uSockets submodule exists, if not clone it
    if [ ! -d "uSockets" ] || [ -z "$(ls -A uSockets 2>/dev/null)" ]; then
        echo "  Cloning uSockets submodule..."
        if [ -f ".gitmodules" ]; then
            git submodule update --init --recursive > /dev/null 2>&1
        else
            # If not a git submodule, clone directly
            if [ ! -d "uSockets" ]; then
                git clone https://github.com/uNetworking/uSockets.git uSockets > /dev/null 2>&1
            fi
        fi
        if [ $? -ne 0 ] || [ ! -d "uSockets" ] || [ -z "$(ls -A uSockets 2>/dev/null)" ]; then
            echo "  Warning: Could not clone uSockets automatically"
            echo "  Please run: cd thirdparty/uWebSockets && git clone https://github.com/uNetworking/uSockets.git uSockets"
            echo "  Or: cd thirdparty/uWebSockets && git submodule update --init --recursive"
        fi
    fi
    
    # Build uSockets with OpenSSL support
    if [ -d "uSockets" ]; then
        cd uSockets
        # Check if already built (uSockets builds as uSockets.a)
        if [ ! -f "uSockets.a" ] && [ ! -f "libusockets.a" ] && [ ! -f "libusockets.so" ]; then
            # Detect OpenSSL paths (especially needed on macOS)
            OPENSSL_CFLAGS=""
            OPENSSL_LDFLAGS=""
            
            # Check for Homebrew OpenSSL on macOS
            if [[ "$OSTYPE" == "darwin"* ]]; then
                # Try OpenSSL 3 first, then fall back to OpenSSL 1.1
                OPENSSL_PREFIX=$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo "")
                if [ -n "$OPENSSL_PREFIX" ]; then
                    OPENSSL_CFLAGS="-I${OPENSSL_PREFIX}/include"
                    OPENSSL_LDFLAGS="-L${OPENSSL_PREFIX}/lib"
                    echo "  Found OpenSSL at: $OPENSSL_PREFIX"
                fi
            fi
            
            # Build with OpenSSL support
            WITH_OPENSSL=1 CFLAGS="${OPENSSL_CFLAGS}" LDFLAGS="${OPENSSL_LDFLAGS}" make > /dev/null 2>&1
            if [ $? -ne 0 ]; then
                echo "✗ uSockets build failed"
                echo "Running make with verbose output:"
                WITH_OPENSSL=1 CFLAGS="${OPENSSL_CFLAGS}" LDFLAGS="${OPENSSL_LDFLAGS}" make
                exit 1
            fi
            echo "✓ uSockets built successfully"
        else
            echo "✓ uSockets already built"
            # Verify the library exists
            if [ ! -f "uSockets.a" ] && [ ! -f "libusockets.a" ] && [ ! -f "libusockets.so" ]; then
                echo "  Warning: uSockets library file not found, rebuilding..."
                WITH_OPENSSL=1 CFLAGS="${OPENSSL_CFLAGS}" LDFLAGS="${OPENSSL_LDFLAGS}" make > /dev/null 2>&1
            fi
        fi
        cd ..
    else
        echo "  Warning: uSockets directory not found. uWebSockets may be header-only."
    fi
    
    cd "$SCRIPT_DIR/.."
    
    # Step 3: Prepare build directory
    echo "[3/5] Preparing build directory..."
    mkdir -p build
    cd build
    
    # Clean stale CMake cache if it exists from a different source directory
    if [ -f "CMakeCache.txt" ]; then
        echo "  Cleaning stale CMake cache..."
        rm -f CMakeCache.txt
        rm -rf CMakeFiles/
    fi
    
    # Step 4: Configure CMake
    echo "[4/5] Configuring CMake..."
    cmake .. > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ CMake configuration failed"
        echo "Running CMake with verbose output:"
        cmake ..
        exit 1
    fi
    echo "✓ CMake configuration successful"
    
    # Step 5: Compile project
    echo "[5/5] Compiling project..."
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
    echo "Executable:"
    echo "  - build/apps/websocket_server"
    echo "=========================================="
    exit 0
fi

# RUN MODE: Run the WebSocket server
cd build

# Check if executable exists
if [ ! -f "./apps/websocket_server" ]; then
    echo "✗ Executable not found"
    echo "Please build the project first by running: ./scripts/start.sh build"
    exit 1
fi

# Clean up existing JSON output file if it exists
rm -f "../../data/order_book_output.json" 2>/dev/null

# Kill any existing processes on port 9001 (WebSocket port)
lsof -ti:9001 | xargs kill -9 2>/dev/null
sleep 1

# Set config path
export ASSIGNMENT_CONFIG="../config/config.ini"

echo "=========================================="
echo "Starting WebSocket Server"
echo "=========================================="
echo "WebSocket port: 9001"
echo "HTTP server: http://localhost:9001"
echo "Press Ctrl+C to stop"
echo ""

# Run the WebSocket server
./apps/websocket_server
