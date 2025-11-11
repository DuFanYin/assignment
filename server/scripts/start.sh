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
    
    # Step 2: Clone uWebSockets (includes uSockets submodule)
    echo "[2/5] Setting up uWebSockets..."
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
    
    # Parse config.ini to get database settings
    DB_HOST=$(grep "^postgres.host=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "localhost")
    DB_PORT=$(grep "^postgres.port=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "5432")
    DB_NAME=$(grep "^postgres.dbname=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "orderbook")
    DB_USER=$(grep "^postgres.user=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "postgres")
    DB_PASSWORD=$(grep "^postgres.password=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "postgres")
    
    # Export password for psql
    export PGPASSWORD="$DB_PASSWORD"
    
    echo "Database: $DB_NAME"
    echo "Host: $DB_HOST"
    echo "Port: $DB_PORT"
    echo "User: $DB_USER"
    echo ""
    
    # Check if PostgreSQL is running
    if ! pg_isready -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" > /dev/null 2>&1; then
        echo "✗ PostgreSQL is not running or not accessible"
        echo "  Please ensure PostgreSQL is running and accessible at $DB_HOST:$DB_PORT"
        exit 1
    fi
    echo "✓ PostgreSQL is running"
    
    # Check if database exists
    DB_EXISTS=$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -lqt | cut -d \| -f 1 | grep -w "$DB_NAME" | wc -l)
    if [ "$DB_EXISTS" -eq 0 ]; then
        echo "✓ Database '$DB_NAME' does not exist (nothing to clean)"
        exit 0
    fi
    
    echo ""
    echo "Wiping database..."
    
    # Drop all tables
    echo "  Dropping tables..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" << EOF > /dev/null 2>&1
DROP TABLE IF EXISTS order_book_snapshots CASCADE;
DROP TABLE IF EXISTS processing_sessions CASCADE;
EOF
    
    if [ $? -ne 0 ]; then
        echo "✗ Failed to drop tables"
        exit 1
    fi
    echo "✓ Tables dropped"
    
    # Recreate schema
    SCHEMA_FILE="db/schema/schema.sql"
    if [ ! -f "$SCHEMA_FILE" ]; then
        echo "✗ Schema file not found: $SCHEMA_FILE"
        exit 1
    fi
    
    echo "  Recreating schema..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -f "$SCHEMA_FILE" > /dev/null 2>&1
    
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

# Parse config.ini to get database settings
DB_HOST=$(grep "^postgres.host=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "localhost")
DB_PORT=$(grep "^postgres.port=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "5432")
DB_NAME=$(grep "^postgres.dbname=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "orderbook")
DB_USER=$(grep "^postgres.user=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "postgres")
DB_PASSWORD=$(grep "^postgres.password=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d ' ' || echo "postgres")

# Export password for psql
export PGPASSWORD="$DB_PASSWORD"

# Check if PostgreSQL is running
if ! pg_isready -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" > /dev/null 2>&1; then
    echo "✗ PostgreSQL not accessible at $DB_HOST:$DB_PORT"
    exit 1
fi

# Check if database exists, create if not
DB_EXISTS=$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -lqt | cut -d \| -f 1 | grep -w "$DB_NAME" | wc -l)
if [ "$DB_EXISTS" -eq 0 ]; then
    echo "Creating database '$DB_NAME'..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d postgres -c "CREATE DATABASE $DB_NAME;" > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "✗ Failed to create database"
        exit 1
    fi
fi

# Check if tables exist, create if not
SCHEMA_FILE="../db/schema/schema.sql"
if [ ! -f "$SCHEMA_FILE" ]; then
    echo "✗ Schema file not found: $SCHEMA_FILE"
    exit 1
fi

TABLE_EXISTS=$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -tAc "SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'processing_sessions');")
if [ "$TABLE_EXISTS" != "t" ]; then
    echo "Creating schema..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -f "$SCHEMA_FILE" > /dev/null 2>&1
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

