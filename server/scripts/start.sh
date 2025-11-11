#!/bin/bash

# WebSocket Server - Build and Run Script
# Usage: ./start.sh [build|run|clean|all]
#   build:       Build databento-cpp, uWebSockets/uSockets, and the project
#   run:         Run the WebSocket server (default, assumes build already exists)
#   clean:       Wipe the database (drop and recreate all tables)
#   all:         Build project, clean database, and run server

MODE=${1:-run}

if [[ "$MODE" != "build" && "$MODE" != "run" && "$MODE" != "clean" && "$MODE" != "all" ]]; then
    echo "Usage: $0 [build|run|clean|all]"
    echo "  build:       Build databento-cpp, uWebSockets/uSockets, and the project"
    echo "  run:         Run the WebSocket server (default, assumes build already exists)"
    echo "  clean:       Wipe the database (drop and recreate all tables)"
    echo "  all:         Build project, clean database, and run server"
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
    
    # Step 0: Check required system libraries for ClickHouse (lz4, cityhash)
    echo "[0/6] Checking system libraries (lz4, cityhash)..."
    MISSING_LIBS=0
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS - prefer Homebrew checks
        if ! command -v brew >/dev/null 2>&1; then
            echo "  Homebrew not found. Please install lz4 and cityhash manually."
        fi
        if ! brew list --versions lz4 >/dev/null 2>&1; then
            echo "  Missing: lz4 (install with: brew install lz4)"
            MISSING_LIBS=1
        fi
        if ! brew list --versions cityhash >/dev/null 2>&1; then
            echo "  Missing: cityhash (install with: brew install cityhash)"
            MISSING_LIBS=1
        fi
    else
        # Linux - try pkg-config and ldconfig
        if ! pkg-config --exists liblz4 2>/dev/null && ! ldconfig -p 2>/dev/null | grep -qi "liblz4"; then
            echo "  Missing: lz4 development package (e.g., apt-get install liblz4-dev or dnf install lz4-devel)"
            MISSING_LIBS=1
        fi
        if ! pkg-config --exists cityhash 2>/dev/null && ! ldconfig -p 2>/dev/null | grep -qi "libcityhash"; then
            echo "  Missing: cityhash development package (e.g., apt-get install libleveldb-dev cityhash or build from source)"
            MISSING_LIBS=1
        fi
    fi
    if [[ "$MISSING_LIBS" -ne 0 ]]; then
        echo "✗ Required system libraries are missing. Please install the above and re-run."
        exit 1
    else
        echo "✓ System libraries present"
    fi
    
    # Step 1: Clone and build databento-cpp
    echo "[1/5] Setting up databento-cpp..."
    mkdir -p thirdparty
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
    
    # Step 2: Clone and build clickhouse-cpp
    echo "[2/6] Setting up clickhouse-cpp..."
    cd thirdparty
    
    if [ ! -d "clickhouse-cpp" ]; then
        echo "  Cloning clickhouse-cpp..."
        git clone https://github.com/ClickHouse/clickhouse-cpp.git > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ Failed to clone clickhouse-cpp"
            exit 1
        fi
    fi
    
    cd clickhouse-cpp
    
    # Build clickhouse-cpp if not already built
    if [ ! -d "build" ] || [ ! -f "build/clickhouse/libclickhouse-cpp-lib.a" ]; then
        echo "  Building clickhouse-cpp..."
        mkdir -p build
        cd build
        cmake .. -DBUILD_SHARED_LIBS=OFF > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ clickhouse-cpp CMake configuration failed"
            echo "Running CMake with verbose output:"
            cmake .. -DBUILD_SHARED_LIBS=OFF
            exit 1
        fi
        make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ clickhouse-cpp build failed"
            echo "Running make with verbose output:"
            make
            exit 1
        fi
        echo "✓ clickhouse-cpp built successfully"
    else
        echo "✓ clickhouse-cpp already built"
    fi
    
    cd "$SCRIPT_DIR/.."
    
    # Step 3: Clone uWebSockets (includes uSockets submodule)
    echo "[3/6] Setting up uWebSockets..."
    cd thirdparty
    
    # Clone uWebSockets with submodules if it doesn't exist
    if [ ! -d "uWebSockets" ] || [ -z "$(ls -A uWebSockets 2>/dev/null)" ]; then
        echo "  Cloning uWebSockets..."
        git clone --recursive https://github.com/uNetworking/uWebSockets.git > /dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "✗ Failed to clone uWebSockets"
            exit 1
        fi
    else
        # If uWebSockets exists but uSockets submodule is missing, initialize it
        cd uWebSockets
        if [ ! -d "uSockets" ] || [ -z "$(ls -A uSockets 2>/dev/null)" ]; then
            echo "  Initializing uSockets submodule..."
            git submodule update --init --recursive > /dev/null 2>&1
            if [ $? -ne 0 ]; then
                echo "  Warning: Could not initialize uSockets submodule"
                echo "  Please run: cd thirdparty/uWebSockets && git submodule update --init --recursive"
            fi
        fi
        cd ..
    fi
    
    cd uWebSockets
    
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
    
    # Step 4: Prepare build directory
    echo "[4/6] Preparing build directory..."
    mkdir -p build
    cd build
    
    # Clean stale CMake cache if it exists from a different source directory
    if [ -f "CMakeCache.txt" ]; then
        echo "  Cleaning stale CMake cache..."
        rm -f CMakeCache.txt
        rm -rf CMakeFiles/
    fi
    
    # Step 5: Configure CMake
    echo "[5/6] Configuring CMake..."
    cmake .. > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ CMake configuration failed"
        echo "Running CMake with verbose output:"
        cmake ..
        exit 1
    fi
    echo "✓ CMake configuration successful"
    
    # Step 6: Compile project
    echo "[6/6] Compiling project..."
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

# CLEAN MODE: Wipe the database
if [[ "$MODE" == "clean" ]]; then
    echo "=========================================="
    echo "WebSocket Server - Clean Database"
    echo "=========================================="
    echo ""
    
    # Read database config from config.ini
    CONFIG_FILE="config/config.ini"
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "✗ Config file not found: $CONFIG_FILE"
        exit 1
    fi
    
    # Parse config.ini to get ClickHouse settings
    DB_HOST=$(grep "^clickhouse.host=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "localhost")
    DB_PORT=$(grep "^clickhouse.port=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "9000")
    DB_NAME=$(grep "^clickhouse.database=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "orderbook")
    DB_USER=$(grep "^clickhouse.user=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "default")
    DB_PASSWORD=$(grep "^clickhouse.password=" "$CONFIG_FILE" | cut -d'=' -f2- || echo "")
    
    echo "Database: $DB_NAME"
    echo "Host: $DB_HOST"
    echo "Port: $DB_PORT"
    echo "User: $DB_USER"
    echo ""
    
    # Check if ClickHouse is running (test query via native client only)
    if ! command -v clickhouse-client >/dev/null 2>&1; then
        echo "✗ clickhouse-client not found. Please install ClickHouse client."
        exit 1
    fi
    CLICKHOUSE_ARGS=(--host="$DB_HOST" --port="$DB_PORT" --user="$DB_USER")
    if [ -n "$DB_PASSWORD" ]; then
        CLICKHOUSE_ARGS+=(--password="$DB_PASSWORD")
    fi
    if ! clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="SELECT 1" > /dev/null 2>&1; then
        echo "✗ ClickHouse is not running or accessible via native protocol at $DB_HOST:$DB_PORT"
        echo "  Please ensure the server is running and accessible."
        exit 1
    fi
    echo "✓ ClickHouse is running"
    
    echo ""
    
    # Ensure database exists
    if ! clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="CREATE DATABASE IF NOT EXISTS $DB_NAME" > /dev/null 2>&1; then
        echo "✗ Failed to create or access database '$DB_NAME'"
        exit 1
    fi
    
    echo "Wiping database..."
    
    # Drop all tables
    echo "  Dropping tables..."
    clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="DROP TABLE IF EXISTS ${DB_NAME}.order_book_snapshots" > /dev/null 2>&1
    clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="DROP TABLE IF EXISTS ${DB_NAME}.processing_sessions" > /dev/null 2>&1
    
    if [ $? -ne 0 ]; then
        echo "✗ Failed to drop tables"
        exit 1
    fi
    echo "✓ Tables dropped"
    
    # Recreate schema
    SCHEMA_FILE="db/schema/clickhouse_schema.sql"
    if [ ! -f "$SCHEMA_FILE" ]; then
        echo "✗ Schema file not found: $SCHEMA_FILE"
        exit 1
    fi
    
    echo "  Recreating schema..."
    clickhouse-client "${CLICKHOUSE_ARGS[@]}" --database="$DB_NAME" --multiquery < "$SCHEMA_FILE" > /dev/null 2>&1
    
    if [ $? -ne 0 ]; then
        echo "✗ Failed to recreate schema"
        exit 1
    fi
    echo "✓ Schema recreated"
    
    echo ""
    echo "=========================================="
    echo "✓ Database cleaned successfully"
    echo "=========================================="
    exit 0
fi

# ALL MODE: Build, clean database, and run server
if [[ "$MODE" == "all" ]]; then
    echo "=========================================="
    echo "WebSocket Server - Build + Clean + Run"
    echo "=========================================="
    echo ""
    
    # First run build
    "$0" build
    if [ $? -ne 0 ]; then
        echo "✗ Build failed, skipping clean and run"
        exit 1
    fi
    
    echo ""
    echo "Build complete, now cleaning database..."
    echo ""
    
    # Then run clean
    "$0" clean
    if [ $? -ne 0 ]; then
        echo "✗ Database clean failed, skipping run"
        exit 1
    fi
    
    echo ""
    echo "Database cleaned, now starting server..."
    echo ""
    
    # Finally run the server
    MODE="run"
    # Fall through to RUN MODE below
fi

# RUN MODE: Run the WebSocket server
cd build

# Check if executable exists
if [ ! -f "./apps/websocket_server" ]; then
    echo "✗ Executable not found"
    echo "Please build the project first by running: ./scripts/start.sh build"
    exit 1
fi

# Set config path
export ASSIGNMENT_CONFIG="../config/config.ini"

# Database setup: Check and create database/tables if needed
echo "=========================================="
echo "Database Setup"
echo "=========================================="

# Read database config from config.ini
CONFIG_FILE="../config/config.ini"
if [ ! -f "$CONFIG_FILE" ]; then
    echo "✗ Config file not found: $CONFIG_FILE"
    exit 1
fi

# Parse config.ini to get ClickHouse settings
DB_HOST=$(grep "^clickhouse.host=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "localhost")
DB_PORT=$(grep "^clickhouse.port=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "9000")
DB_NAME=$(grep "^clickhouse.database=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "orderbook")
DB_USER=$(grep "^clickhouse.user=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "default")
DB_PASSWORD=$(grep "^clickhouse.password=" "$CONFIG_FILE" | cut -d'=' -f2- || echo "")

# Prepare client args
if ! command -v clickhouse-client >/dev/null 2>&1; then
    echo "✗ clickhouse-client not found. Please install ClickHouse client."
    exit 1
fi
CLICKHOUSE_ARGS=(--host="$DB_HOST" --port="$DB_PORT" --user="$DB_USER")
if [ -n "$DB_PASSWORD" ]; then
    CLICKHOUSE_ARGS+=(--password="$DB_PASSWORD")
fi

# Ensure database exists
if ! clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="CREATE DATABASE IF NOT EXISTS $DB_NAME" > /dev/null 2>&1; then
    echo "✗ Failed to create or access database '$DB_NAME'"
    exit 1
fi

# Ensure schema exists (if not present, apply)
SCHEMA_FILE="../db/schema/clickhouse_schema.sql"
if [ ! -f "$SCHEMA_FILE" ]; then
    echo "✗ Schema file not found: $SCHEMA_FILE"
    exit 1
fi

TABLE_EXISTS=$(clickhouse-client "${CLICKHOUSE_ARGS[@]}" --query="SELECT count() FROM system.tables WHERE database = '$DB_NAME' AND name = 'processing_sessions'")
if [ "$TABLE_EXISTS" -eq 0 ]; then
    echo "Creating schema..."
    clickhouse-client "${CLICKHOUSE_ARGS[@]}" --database="$DB_NAME" --multiquery < "$SCHEMA_FILE" > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ Failed to create schema"
        exit 1
    fi
fi

echo "✓ Database ready: $DB_NAME @ $DB_HOST:$DB_PORT"
echo ""

# Kill any existing processes on port 9001
lsof -ti:9001 | xargs kill -9 2>/dev/null
sleep 1

echo "=========================================="
echo "Starting Server"
echo "=========================================="
echo "HTTP: http://localhost:9001"
echo "Press Ctrl+C to stop"
echo "=========================================="
echo ""

# Run the WebSocket server
./apps/websocket_server

