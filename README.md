# WebSocket Order Book Server

## Prerequisites & Setup

### Required Software

Before building and running the server, ensure you have the following installed:

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

### Threading Model

The server uses a multi-threaded architecture optimized for performance:

```
┌─────────────────────────────────────────────────────────────┐
│                      Main Thread                            │
│  - uWebSockets event loop (WebSocket + HTTP)                │
│  - Accepts file uploads                                     │
│  - Serves static files                                      │
│  - Handles download requests                                │
└────────────────┬────────────────────────────────────────────┘
                 │
                 ├─► Processing Thread (jthread, detached)
                 │   - Reads DBN file via DbnFileStore
                 │   - Applies MBO messages to order book
                 │   - Captures snapshots after each update
                 │   - Pushes snapshots to ring buffer
                 │   - Waits for DB writes to complete
                 │   - Sends completion message to client
                 │
                 └─► Database Writer Thread (jthread)
                     - Drops indexes for faster bulk loading
                     - Pops snapshots from ring buffer
                     - Writes snapshots via PostgreSQL COPY
                     - Recreates indexes after bulk load
                     - Updates session stats and ends session
```

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

1. **User uploads DBN file** through web interface
2. **Server receives file** via WebSocket binary messages
3. **Processing thread spawns** to handle the file:
   - Reads DBN records using `DbnFileStore`
   - Applies each MBO message to order book
   - Captures snapshot (BBO, top-N levels, statistics)
   - Pushes snapshot to lock-free ring buffer
4. **Database writer thread** (running continuously):
   - Drops indexes at start of session for faster bulk loading
   - Pops snapshots from ring buffer in batches (5000 items)
   - Writes snapshots to PostgreSQL using COPY commands
   - Recreates indexes after all snapshots are written
   - Updates session statistics and final book state
   - Ends database session
5. **Processing thread waits** for database writer to complete
6. **Completion message sent** to frontend (everything is done)
7. **User downloads JSON** via download button (queries database on-demand)


## Implementation Details

### Thread Synchronization & Delegation Architecture

**Processing Thread → Database Thread Delegation:**

The key architectural pattern is **delegation via ring buffer**:

1. **Processing Thread** (hot path, performance-critical, `std::jthread`):
   - Reads DBN file and applies MBO messages to order book
   - Captures snapshot after each message
   - **Pushes snapshot to lock-free ring buffer** (non-blocking when space available)
   - **Never writes to database directly** - pure in-memory operations
   - **Waits for DB thread to complete** before sending completion message
   - Captures session statistics into `SessionStats` struct with memory fences
   - Uses `std::jthread` for consistency with database writer thread (detached after spawn)

2. **Database Writer Thread** (background, I/O operations, `std::jthread`):
   - **Drops indexes** at start of session for faster bulk loading
   - **Pops snapshots from ring buffer** in batches (blocking wait when empty)
   - Writes snapshots to PostgreSQL using **COPY commands** for maximum performance
   - **Recreates indexes** after all snapshots are written
   - Updates session statistics and final book state
   - Runs continuously until server stops AND ring buffer is empty
   - Uses `std::jthread` with stop token for graceful shutdown

**Ring Buffer Implementation:**
- **Lock-free SPSC** (Single Producer Single Consumer) queue
- C++20 atomics with `wait/notify` for efficient blocking
- Producer pushes, consumer pops - no mutex contention
- Power-of-2 size with bitmask indexing for fast modulo
- Cache-line aligned read/write positions to avoid false sharing

**Database Session Management:**
- Each upload creates a unique session ID in database
- Session tracks: file metadata, statistics, snapshots, final state
- Multiple sessions can exist concurrently in database
- Session ID is returned to frontend for JSON download

**JSON Generation:**
- **On-demand** generation (not during processing)
- Queries database by session_id to retrieve snapshots
- Generates newline-delimited JSON format
- Served via HTTP endpoint with session-specific download

### Performance Optimizations

1. **PostgreSQL COPY commands**: Bulk data loading using COPY instead of individual INSERTs for maximum throughput
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