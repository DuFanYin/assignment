# WebSocket Order Book Server

A high-performance server for processing DBN (Databento) order book files, storing snapshots in ClickHouse, and serving JSON output on-demand.

## Prerequisites & Setup

### Required Software

**Build Tools:**
- **CMake 3.16+** - Build system
- **C++20 compiler** - GCC 10+ or Clang 12+ with C++20 support

**System Libraries:**
- **lz4** - Compression library required by ClickHouse
- **cityhash** - Hash library required by ClickHouse
- **OpenSSL** - For secure connections
- **ZLIB** - For compression
- **CURL** - For HTTP client functionality

**Runtime Requirements:**
- **ClickHouse** - Columnar database server must be installed and running
- **clickhouse-client** - ClickHouse command-line client (for database setup)
- **Git** - For cloning third-party dependencies (only needed for first build)

### Installation

**macOS:**
```bash
brew install clickhouse lz4 cityhash cmake openssl zlib curl
```

**Ubuntu/Debian:**
```bash
# Install ClickHouse (see https://clickhouse.com/docs/en/install)
sudo apt-get install clickhouse-server clickhouse-client
sudo apt-get install liblz4-dev libcityhash-dev cmake libssl-dev zlib1g-dev libcurl4-openssl-dev build-essential git
```

**Fedora/RHEL:**
```bash
# Install ClickHouse (see https://clickhouse.com/docs/en/install)
sudo dnf install clickhouse-server clickhouse-client
sudo dnf install lz4-devel cityhash-devel cmake openssl-devel zlib-devel libcurl-devel gcc-c++ git
```

### Database Setup

The `server/scripts/start.sh` script automatically handles all database setup. You only need to:

1. **Ensure ClickHouse is running** - The script will verify this automatically
2. **Configure database connection** in `server/config/config.ini` (see Configuration section below)

The script will automatically:
- Check if ClickHouse is accessible via native protocol
- Create the database if it doesn't exist
- Create all required tables if they don't exist

**No manual database setup is required!**

The build script will also automatically download and build third-party dependencies (databento-cpp, clickhouse-cpp, uWebSockets) on first build.

### Configuration

Edit `server/config/config.ini` to configure server and database settings:

```ini
# WebSocket server settings
websocket.port=9001

# Order book settings
# Note: symbol is extracted from the DBN file metadata automatically
server.top_levels=10
server.ring_buffer_size=65536

# ClickHouse settings (high-performance columnar database)
clickhouse.host=127.0.0.1
clickhouse.port=9000
clickhouse.database=orderbook
clickhouse.user=default
clickhouse.password=your_password
clickhouse.compression=true
```

**Important:** Ensure ClickHouse is running before starting the server. The script will automatically create the database and tables if they don't exist.

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

The server processes DBN files containing market-by-order (MBO) messages, maintains an in-memory order book, captures snapshots after each update, and stores them in ClickHouse. The architecture uses a per-cycle threading model where each file upload spawns dedicated processing and database writer threads that run to completion.

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
                     - Pops snapshots from ring buffer
                     - Writes snapshots via ClickHouse Block inserts
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
            │ (5) Write via ClickHouse Block insert
            │ (6) Update session stats
            ▼
    ClickHouse Database
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
   - Pops snapshots from ring buffer in batches
   - Writes snapshots to ClickHouse using native Block inserts (columnar format)
   - ClickHouse automatically handles indexing (sparse indexes, no manual management needed)
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
   - Pops snapshots from ring buffer in batches (blocking wait when empty)
   - Writes snapshots to ClickHouse using native Block inserts (columnar format)
   - ClickHouse uses sparse indexes automatically - no manual index management needed
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

### Performance Optimizations

**Threading & Concurrency:**
- Lock-free SPSC ring buffer with C++20 atomics (wait/notify)
- Cache-line aligned read/write positions (64-byte alignment)
- Power-of-2 ring buffer size for fast modulo via bitmask
- Memory fences for proper cross-thread synchronization
- Processing thread never blocks on I/O (pure in-memory operations)

**Database Performance:**
- **Native Array Storage**: ClickHouse native `Array(Tuple(...))` types instead of JSONB (no parsing overhead)
- **Columnar Storage**: MergeTree engine optimized for time-series data with automatic compression
- **Block Inserts**: Native ClickHouse Block API for efficient bulk inserts
- **Automatic Indexing**: Sparse indexes managed automatically by ClickHouse (no manual index management)
- **No WAL Overhead**: ClickHouse's columnar design eliminates traditional WAL bottlenecks
- Batch processing in large chunks for optimal throughput

**Memory & CPU:**
- Efficient snapshot structure with minimal heap allocations
- Fixed-size top-N levels (configured via `server.top_levels`)
- On-demand JSON generation (only when user downloads)
- Modern C++20 jthread for automatic thread cleanup

## Database Schema

**`processing_sessions`**
- Tracks each upload session with metadata and statistics
- Engine: `MergeTree()` ordered by `(session_id, start_time)`
- Fields: session_id, symbol, file_name, file_size, status, timestamps, statistics, final book state

**`order_book_snapshots`**
- Stores order book state after each MBO message
- Engine: `MergeTree()` ordered by `(session_id, timestamp_ns)` with monthly partitioning
- Level data stored as native ClickHouse arrays: `Array(Tuple(price Int64, size UInt32, count UInt32))`
- No foreign key constraints (ClickHouse doesn't enforce them, but session_id links to processing_sessions)
- Sparse index on `symbol` for faster queries

**Price Format:**
- Fixed-point: multiply by 1e-9 for decimal value
- Sizes in native instrument units
- Counts = number of orders at that level



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

