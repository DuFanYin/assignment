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

### Start Server

Use the convenience script:

```bash
cd server
./scripts/start.sh build    # First time: build dependencies and project
./scripts/start.sh run      # Subsequent runs: just start the server
```

## Architecture


### Threading Model

The server uses a per-cycle multi-threaded architecture with **true streaming processing**:


**Cycle Lifecycle:**
- Client sends metadata (fileSize + fileName) → Processing thread starts immediately
- File chunks arrive via WebSocket → Appended to streaming buffer
- Processing thread consumes chunks as they arrive (true streaming)
- Database writer thread spawns at cycle start
- Both threads run to completion
- Next cycle can start after previous cycle completes

WebSocket Thread (Event Loop)
    ↓ (writes binary chunks)
StreamingBuffer (shared buffer)
    ↓ (reads chunks)
Processing Thread
    ↓ (pushes snapshots)
RingBuffer<MboMessageWrapper>
    ↓ (pops snapshots)
Database Writer Thread
    ↓ (batch inserts)
ClickHouse Database

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

## Performance Metrics

The server reports detailed performance metrics after each file processing session. These metrics help understand where time is spent and identify bottlenecks:

### Throughput Metrics

**Total Throughput (entire session)** - `totalThroughput` (msg/s)
- **What it measures**: End-to-end message processing rate from upload start to database completion
- **Time window**: From metadata arrival (`uploadStartTime_`) through database writer thread completion (`dbEndTime_`)
- **Formula**: `processingMessagesReceived / (dbEndTime - uploadStartTime)`
- **Use case**: Overall system performance indicator - lower values indicate bottlenecks in upload, processing, or database writes

**Upload Throughput (Network I/O speed)** - `uploadThroughputMBps` (MB/s)
- **What it measures**: Network transfer rate - how fast file data is received over WebSocket in megabytes per second
- **Time window**: From metadata arrival (`uploadStartTime_`) to final chunk received (`uploadEndTime_`)
- **Formula**: `(uploadBytesReceived / (1024 * 1024)) / (uploadEndTime - uploadStartTime)`
- **Use case**: Measures network bandwidth utilization and upload efficiency. Shows actual data transfer speed. Affected by network speed, chunk size, and WebSocket overhead. Typical localhost speeds: 200-1000+ MB/s

**Order Throughput (order processing speed)** - `orderThroughput` (orders/s)
- **What it measures**: Order book update rate - how fast MBO messages are applied to the order book
- **Time window**: From first record decoded (`processingStartTime_`) to last record processed (`processingEndTime_`)
- **Formula**: `processingOrdersProcessed / (processingEndTime - processingStartTime)`
- **Use case**: Measures CPU efficiency of order book operations. Higher values indicate faster order book updates

**DB Throughput (file I/O speed)** - `dbThroughput` (items/s)
- **What it measures**: Database write rate - how fast snapshots are written to ClickHouse
- **Time window**: From first snapshot written (`dbStartTime_`) to last snapshot written (`dbEndTime_`)
- **Formula**: `snapshotsWritten / (dbEndTime - dbStartTime)`
- **Use case**: Measures database write performance. Affected by ClickHouse performance, batch size, and network latency to database

### Latency Metrics

**Average Process Time** - `averageOrderProcessNs` (nanoseconds)
- **What it measures**: Average time to process a single MBO message (apply to order book + capture snapshot)
- **Calculation**: Mean of all individual order processing times
- **Formula**: `totalProcessingTimeNs / processingTimingSamples`
- **Use case**: Typical order processing latency. Lower values indicate faster order book operations

**P99 Process Time** - `p99OrderProcessNs` (nanoseconds)
- **What it measures**: 99th percentile processing time - 99% of orders are processed faster than this value
- **Calculation**: Uses reservoir sampling to estimate P99 from timing samples
- **Use case**: Identifies worst-case latency outliers. High P99 values indicate occasional slow operations (e.g., order book rebalancing)

### Understanding the Metrics

**Typical Performance Profile:**
- **Upload Throughput** is usually the fastest (network is fast, especially on localhost)
- **Order Throughput** is typically slower than upload (CPU-bound order book operations)
- **DB Throughput** may be slower if database is remote or under load
- **Total Throughput** is the slowest (includes all phases: upload + processing + database writes)

**Note**: All timestamps use `std::chrono::steady_clock` for high-precision, monotonic timing that is unaffected by system clock adjustments.

