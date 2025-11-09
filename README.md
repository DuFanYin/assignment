# WebSocket Order Book Server

A high-performance server for processing DBN (Databento) order book files, storing snapshots in PostgreSQL, and serving JSON output on-demand.

## Prerequisites & Setup

### Required Software

**Build Tools:**
- **CMake 3.16+** - Build system
- **C++20 compiler** - GCC 10+ or Clang 12+ with C++20 support

**System Libraries:**
- **PostgreSQL development libraries** (`libpq`) - For database connectivity
- **OpenSSL** - For secure connections
- **ZLIB** - For compression
- **CURL** - For HTTP client functionality

**Runtime Requirements:**
- **PostgreSQL 12+** - Database server must be installed and running
- **Git** - For cloning third-party dependencies (only needed for first build)

### Installation

**macOS:**
```bash
brew install postgresql cmake openssl zlib curl
```

**Ubuntu/Debian:**
```bash
sudo apt-get install postgresql postgresql-dev cmake libssl-dev zlib1g-dev libcurl4-openssl-dev build-essential git
```

**Fedora/RHEL:**
```bash
sudo dnf install postgresql postgresql-devel cmake openssl-devel zlib-devel libcurl-devel gcc-c++ git
```

### Database Setup

The `server/scripts/start.sh` script automatically handles all database setup. You only need to:

1. **Ensure PostgreSQL is running** - The script will verify this automatically
2. **Configure database connection** in `server/config/config.ini` (see Configuration section below)

The script will automatically:
- Check if PostgreSQL is accessible
- Create the database if it doesn't exist
- Create all required tables if they don't exist

**No manual database setup is required!**

The build script will also automatically download and build third-party dependencies (databento-cpp, uWebSockets) on first build.

### Configuration

Edit `server/config/config.ini` to configure server and database settings:

```ini
# WebSocket server settings
websocket.port=9001

# Order book settings
server.top_levels=10
server.output_full_book=true
server.ring_buffer_size=65536

# PostgreSQL settings (required)
postgres.host=localhost
postgres.port=5432
postgres.dbname=orderbook
postgres.user=postgres
postgres.password=your_password
postgres.max_connections=10
postgres.connection_timeout=30
```

**Important:** Ensure PostgreSQL is running before starting the server. The script will automatically create the database and tables if they don't exist.

### Start Server

Use the convenience script:

```bash
cd server
./scripts/start.sh build    # First time: build dependencies and project
./scripts/start.sh run      # Subsequent runs: just start the server
```

Or from the project root:

```bash
./server/scripts/start.sh build    # First time: build dependencies and project
./server/scripts/start.sh run      # Subsequent runs: just start the server
```

## Architecture

### Overview

The server processes DBN files containing market-by-order (MBO) messages, maintains an in-memory order book, captures snapshots after each update, and stores them in PostgreSQL. The architecture uses a per-cycle threading model where each file upload spawns dedicated processing and database writer threads that run to completion.

### Threading Model

The server uses a per-cycle multi-threaded architecture:

```
┌─────────────────────────────────────────────────────────────┐
│                      Main Thread                            │
│  - uWebSockets event loop (WebSocket + HTTP)                │
│  - Accepts file uploads                                     │
│  - Serves static files                                      │
│  - Handles download requests                                │
└────────────────┬────────────────────────────────────────────┘
                 │
                 │ (per file upload cycle)
                 │
                 ├─► Processing Thread (jthread, stored)
                 │   - Reads DBN file via DbnFileStore
                 │   - Applies MBO messages to order book
                 │   - Captures snapshots after each update
                 │   - Pushes snapshots to ring buffer
                 │   - Waits for DB writes to complete
                 │   - Sends completion message to client
                 │   - Cannot be stopped once started
                 │
                 └─► Database Writer Thread (jthread, per-cycle)
                     - Drops indexes for faster bulk loading
                     - Pops snapshots from ring buffer
                     - Writes snapshots via PostgreSQL COPY
                     - Recreates indexes after bulk load
                     - Updates session stats and ends session
                     - Exits after cycle completes
```

**Cycle Lifecycle:**
- Each file upload starts a new cycle
- Processing thread spawns → Database writer thread spawns
- Both threads run to completion
- Next cycle can start after previous cycle completes

### Data Flow

```
User Browser
    │
    │ (1) Upload DBN file via WebSocket
    ▼
WebSocket Server
    │
    │ (2) Write to temp file
    ▼
Processing Thread
    │
    ├─► DbnFileStore (parse DBN)
    │
    ├─► Order Book (apply MBO messages)
    │
    └─► Capture BookSnapshot
            │
            │ (3) Push to Ring Buffer
            ▼
        Ring Buffer (SPSC, lock-free)
            │
            │ (4) Pop snapshots in batches
            ▼
    Database Writer Thread
            │
            │ (5) Drop indexes
            │ (6) Write via COPY command
            │ (7) Recreate indexes
            │ (8) Update session stats
            ▼
    PostgreSQL Database
            │
            │ (9) User requests download via HTTP
            ▼
    JSON Generator
            │
            │ (10) Query database & generate JSON
            ▼
        User Browser
```

### Workflow

**Per-Cycle Processing:**

1. **User uploads DBN file** through web interface
2. **Server receives file** via WebSocket binary messages and writes to temporary file
3. **Processing thread spawns** when file upload completes:
   - Reads DBN records using `DbnFileStore`
   - Applies each MBO message to order book
   - Captures snapshot (BBO, top-N levels, statistics) after each update
   - Pushes snapshot to lock-free ring buffer
   - Cannot be stopped once started
4. **Database writer thread spawns** at the start of the cycle:
   - Drops indexes at start of session for faster bulk loading
   - Pops snapshots from ring buffer in batches (5000 items)
   - Writes snapshots to PostgreSQL using COPY commands
   - Recreates indexes after all snapshots are written
   - Updates session statistics and final book state
   - Ends database session
   - Exits after cycle completes
5. **Processing thread waits** for database writer to complete
6. **Completion message sent** to frontend (everything is done)
7. **User downloads JSON** via download button (queries database on-demand)
8. **Next cycle** can start when user uploads another file

## Implementation Details

### Thread Synchronization & Delegation

The architecture uses **delegation via ring buffer** to separate processing from database I/O:

1. **Processing Thread** (`std::optional<std::jthread>`):
   - Spawns per-cycle when file upload completes
   - Reads DBN file and applies MBO messages to order book
   - Captures snapshot after each message
   - Pushes snapshot to lock-free ring buffer (non-blocking when space available)
   - Never writes to database directly - pure in-memory operations
   - Waits for DB thread to complete before sending completion message
   - Captures session statistics into `SessionStats` struct with memory fences
   - Stored in `processingThread_` member variable for proper lifecycle management
   - Cannot be stopped once started - runs to completion
   - Clears order book after processing completes

2. **Database Writer Thread** (`std::jthread`, per-cycle):
   - Spawns per-cycle in `startProcessingThread()` before processing thread
   - Drops indexes at start of session for faster bulk loading
   - Pops snapshots from ring buffer in batches (blocking wait when empty)
   - Writes snapshots to PostgreSQL using COPY commands
   - Recreates indexes after all snapshots are written
   - Updates session statistics and final book state
   - Exits after cycle completes (when processing done AND buffer empty)
   - Next cycle spawns a new database writer thread

**Ring Buffer Implementation:**
- Lock-free SPSC (Single Producer Single Consumer) queue
- C++20 atomics with `wait/notify` for efficient blocking
- Producer pushes, consumer pops - no mutex contention
- Power-of-2 size with bitmask indexing for fast modulo
- Cache-line aligned read/write positions to avoid false sharing

**Database Session Management:**
- Each upload cycle creates a unique session ID in database
- Session tracks: file metadata, statistics, snapshots, final state
- Multiple sessions can exist concurrently in database
- Session ID is returned to frontend for JSON download
- Each cycle has its own database writer thread that manages the session lifecycle

**JSON Generation:**
- On-demand generation (not during processing)
- Queries database by session_id to retrieve snapshots
- Generates newline-delimited JSON format
- Served via HTTP endpoint with session-specific download

### Performance Features

1. **PostgreSQL COPY commands**: Bulk data loading using COPY for maximum throughput
2. **Index management**: Drop indexes before bulk load, recreate after - significantly faster than maintaining indexes during inserts
3. **Lock-free ring buffer delegation**: Processing thread never blocks on database I/O - delegates writes to dedicated DB thread via lock-free SPSC queue
4. **Cache-line alignment**: Ring buffer read/write positions on separate 64-byte cache lines to prevent false sharing
5. **Batch processing**: Database writer processes snapshots in batches of 5000 for optimal COPY performance
6. **Separate database writer thread**: Order book processing is pure in-memory - all I/O happens in background
7. **Efficient snapshot structure**: Minimal heap allocations, fixed-size top-N levels
8. **On-demand JSON generation**: JSON generated only when downloaded, not during processing - reduces memory footprint
9. **C++20 atomic wait/notify**: Efficient blocking without busy-waiting or condition variables
10. **Power-of-2 ring buffer**: Fast modulo via bitmask, no division operations
11. **Memory fences**: Proper synchronization between processing and DB threads using `std::atomic_thread_fence`
12. **C++20 jthread**: Both processing and database writer threads use `std::jthread` for modern thread management with stop tokens
13. **Per-cycle thread spawning**: Database writer thread spawned per-cycle for clean session management and resource cleanup
14. **Stored processing thread**: Processing thread stored in `std::optional<std::jthread>` for proper lifecycle management

### Order Book Snapshot Format

Each JSON record contains:

```json
{
  "symbol": "CLX5",
  "timestamp": "1234567890123456789",
  "timestamp_ns": 1234567890123456789,
  "bbo": {
    "bid": {"price": "123450000000", "size": 10, "count": 3},
    "ask": {"price": "123460000000", "size": 8, "count": 2}
  },
  "levels": {
    "bids": [
      {"price": "123450000000", "size": 10, "count": 3},
      {"price": "123440000000", "size": 15, "count": 5}
    ],
    "asks": [
      {"price": "123460000000", "size": 8, "count": 2},
      {"price": "123470000000", "size": 12, "count": 4}
    ]
  },
  "stats": {
    "total_orders": 25,
    "bid_levels": 10,
    "ask_levels": 10
  }
}
```

**Field Descriptions:**
- `symbol`: Instrument symbol from DBN file metadata
- `timestamp`: Event timestamp as string (nanoseconds since epoch)
- `timestamp_ns`: Event timestamp as integer (nanoseconds since epoch)
- `bbo.bid/ask`: Best bid/offer with price, size, and order count
- `levels.bids/asks`: Top-N price levels (N = `top_levels` from config)
- `stats`: Order book statistics at this snapshot

**Price Format:**
- Prices are in fixed-point format (multiply by 1e-9 to get decimal)
- Sizes are in native instrument units
- Counts represent number of orders at that level
