# WebSocket Order Book Server

A high-performance server for processing DBN (Databento) order book files, storing snapshots in ClickHouse, and serving JSON output on-demand.

<p align="center">
  <img src="doc/result.jpg" alt="Example result showing UI and metrics" width="700">
</p>
<p align="center">
  <em><b>Tested on localhost</b></em>
</p>


## Prerequisites & Setup

### Required Software

**Requirements:** CMake 3.16+, C++20 compiler (GCC 10+/Clang 12+), lz4, cityhash, OpenSSL, ZLIB, CURL, ClickHouse server and client, Git

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

```
WebSocket Thread (Event Loop)
    │
    │ writes binary chunks
    ▼
StreamingBuffer (shared buffer)
    │
    │ reads chunks
    ▼
Processing Thread
    │
    │ pushes snapshots
    ▼
RingBuffer<MboMessageWrapper>
    │
    │ pops snapshots
    ▼
Database Writer Thread
    │
    │ batch inserts
    ▼
ClickHouse Database
```

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


### Order Book Snapshot Format

Each JSON record contains:

```json
{
  "symbol": "CLX5",
  "timestamp": "1234567890123456789",
  "timestamp_ns": 1234567890123456789,
  "bbo": {
    "bid": {"price": "64.83", "size": 10, "count": 3},
    "ask": {"price": "64.84", "size": 8, "count": 2}
  },
  "levels": {
    "bids": [
      {"price": "64.83", "size": 10, "count": 3},
      {"price": "64.82", "size": 15, "count": 5}
    ],
    "asks": [
      {"price": "64.84", "size": 8, "count": 2},
      {"price": "64.85", "size": 12, "count": 4}
    ]
  },
  "stats": {
    "total_orders": 25,
    "bid_levels": 10,
    "ask_levels": 10
  }
}
```

## Performance Metrics

The server reports detailed performance metrics after each file processing session. These metrics help understand where time is spent and identify bottlenecks:

### Throughput Metrics

- `totalThroughput` (msg/s): End-to-end message rate measured from metadata arrival (`uploadStartTime_`) through database writer completion (`dbEndTime_`).
- `uploadThroughputMBps` (MB/s): WebSocket upload bandwidth measured from metadata arrival (`uploadStartTime_`) to final chunk received (`uploadEndTime_`).
- `orderThroughput` (orders/s): Order application rate measured from first decoded record (`processingStartTime_`) to last processed record (`processingEndTime_`).
- `dbThroughput` (items/s): Snapshot insert rate measured from first DB write (`dbStartTime_`) to last DB write (`dbEndTime_`).

### Latency Metrics

- `averageOrderProcessNs` (ns): Mean per-order processing time measured between `applyStart` and `applyEnd` for each MBO message.
- `p99OrderProcessNs` (ns): 99th percentile of the same per-order processing timings.

All timestamps use `std::chrono::steady_clock` for high-precision, monotonic timing.

