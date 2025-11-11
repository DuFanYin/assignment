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

The server processes DBN files containing market-by-order (MBO) messages using **true streaming architecture**: processing starts immediately after metadata is received and processes chunks as they arrive over WebSocket. The server maintains an in-memory order book, captures snapshots after each update, and stores them in ClickHouse. The architecture uses a per-cycle threading model where each file upload spawns dedicated processing and database writer threads that run to completion. **No temporary files are used** - all processing happens in memory with parallel upload and processing.

### Threading Model

The server uses a per-cycle multi-threaded architecture with **true streaming processing**:


**Cycle Lifecycle:**
- Client sends metadata (fileSize + fileName) → Processing thread starts immediately
- File chunks arrive via WebSocket → Appended to streaming buffer
- Processing thread consumes chunks as they arrive (true streaming)
- Database writer thread spawns at cycle start
- Both threads run to completion
- Next cycle can start after previous cycle completes

### Data Flow

```
User Browser
    │
    │ (1) Send metadata (fileSize + fileName) via WebSocket
    ▼
WebSocket Server (Main Thread)
    │
    │ (2) Create StreamingBufferState, start Processing Thread
    │
    ├─► Processing Thread (starts immediately)
    │   │
    │   ├─► StreamingReadable → StreamingBufferState
    │   │   │
    │   │   └─► (3) Chunks arrive via WebSocket (parallel)
    │   │       └─► Main Thread appends to StreamingBufferState
    │   │
    │   ├─► DbnDecoder (blocks in ReadSome() until chunks available)
    │   │
    │   ├─► Order Book (apply MBO messages)
    │   │
    │   └─► BookSnapshot → Ring Buffer
    │
    └─► File chunks arrive via WebSocket (parallel with processing)
        └─► Append to StreamingBufferState
            └─► Processing Thread consumes via StreamingReadable

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
    │ (7) User requests download via HTTP
    ▼
JSON Generator
    │
    │ (8) Query database & generate JSON
    ▼
User Browser
```

### Workflow

**Per-Cycle Processing (True Streaming):**

1. **User sends metadata** (fileSize + fileName) via WebSocket binary message
2. **Server initializes streaming state** and **starts processing thread immediately**:
   - Creates `StreamingBufferState` (thread-safe chunk queue)
   - Creates `StreamingReadable` (IReadable adapter for decoder)
   - Spawns processing thread (doesn't wait for file upload)
3. **Processing thread starts decoding** (blocks waiting for chunks):
   - Uses `DbnDecoder` with `StreamingReadable`
   - Decoder calls `ReadSome()` which blocks until chunks arrive
   - Extracts metadata from stream
   - Starts database session
4. **User sends file chunks** via WebSocket (parallel with processing):
   - Chunks appended to `StreamingBufferState`
   - Processing thread consumes chunks as they arrive
   - True streaming: processing happens in parallel with upload
5. **Processing thread processes records**:
   - Applies each MBO message to order book
   - Captures snapshot (BBO, top-N levels, statistics) after each update
   - Pushes snapshot to lock-free ring buffer
   - Cannot be stopped once started
6. **Database writer thread spawns** at cycle start:
   - Pops snapshots from ring buffer in batches
   - Writes snapshots to ClickHouse using native Block inserts (columnar format)
   - ClickHouse automatically handles indexing (sparse indexes, no manual management needed)
   - Updates session statistics and final book state
   - Ends database session
   - Exits after cycle completes
7. **Processing thread waits** for database writer to complete
8. **Completion message sent** to frontend via thread-safe `loop->defer()` (everything is done)
9. **User downloads JSON** via download button (queries database on-demand)
10. **Next cycle** can start when user uploads another file

## Implementation Details

### Thread Synchronization & Delegation

The architecture uses **true streaming** with **delegation via ring buffer** to separate processing from database I/O:

1. **Processing Thread** (`std::optional<std::jthread>`):
   - Spawns per-cycle **immediately after metadata received** (doesn't wait for full file)
   - Uses `StreamingReadable` (implements `databento::IReadable`) to read from `StreamingBufferState`
   - `DbnDecoder` blocks in `ReadSome()` waiting for chunks as they arrive
   - Applies MBO messages to order book as records are decoded
   - Captures snapshot after each message
   - Pushes snapshot to lock-free ring buffer (non-blocking when space available)
   - Never writes to database directly - pure in-memory operations
   - Processing happens **in parallel with file upload** (true streaming)
   - Waits for DB thread to complete before sending completion message
   - Sends status updates via thread-safe `loop->defer()` (schedules on event loop thread)
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

**Streaming Architecture:**
- `StreamingBufferState`: Thread-safe chunk queue using `std::deque<std::vector<std::byte>>`
- Uses mutex + condition variable for producer/consumer synchronization
- WebSocket thread (producer) appends chunks, processing thread (consumer) reads chunks
- `StreamingReadable`: Implements `databento::IReadable` interface
- `ReadSome()` blocks waiting for chunks if buffer is empty (true streaming behavior)
- No temporary files - all processing happens in memory

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
- True streaming: processing starts immediately after metadata, processes chunks as they arrive
- Thread-safe WebSocket messaging via `loop->defer()` (schedules sends on event loop thread)
- Lock-free SPSC ring buffer with C++20 atomics (wait/notify)
- Cache-line aligned read/write positions (64-byte alignment)
- Power-of-2 ring buffer size for fast modulo via bitmask
- Memory fences for proper cross-thread synchronization
- Processing thread blocks only when waiting for chunks (streaming behavior)
- No temporary files - all data stays in memory

**Database Performance:**
- **Native Array Storage**: ClickHouse native `Array(Tuple(...))` types instead of JSONB (no parsing overhead)
- **Columnar Storage**: MergeTree engine optimized for time-series data with automatic compression
- **Block Inserts**: Native ClickHouse Block API for efficient bulk inserts
- **Automatic Indexing**: Sparse indexes managed automatically by ClickHouse (no manual index management)
- **No WAL Overhead**: ClickHouse's columnar design eliminates traditional WAL bottlenecks
- Batch processing in large chunks for optimal throughput

**Memory & CPU:**
- Streaming buffer: chunks stored in `std::deque` (efficient append/consume)
- No temporary file I/O - all processing in memory
- Efficient snapshot structure with minimal heap allocations
- Fixed-size top-N levels (configured via `server.top_levels`)
- On-demand JSON generation (only when user downloads)
- Modern C++20 jthread for automatic thread cleanup
- WebSocket compression disabled (DBN files already compressed)

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

