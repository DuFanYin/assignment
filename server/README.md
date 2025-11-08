# WebSocket Order Book Server

## Overview

This server module implements a high-performance WebSocket server for processing Databento DBN (Market-By-Order) files, maintaining an in-memory order book, and persisting snapshots to PostgreSQL. The server features a modern web interface for file uploads and provides real-time processing statistics.

### Key Features

- **WebSocket-based file upload**: Upload DBN files directly through a web browser
- **Real-time order book processing**: Processes MBO messages and maintains live order book state
- **Database persistence**: Order book snapshots stored in PostgreSQL
- **On-demand JSON generation**: HTTP endpoint to query database and download JSON snapshots
- **Modern web UI**: Clean, responsive interface with drag-and-drop file upload
- **Performance metrics**: Detailed statistics including throughput, processing latency, and P99 measurements

### Architecture Components

- **WebSocketServer** (`src/core/server.cpp`, `include/project/server.hpp`): Main server handling WebSocket connections, file uploads, and HTTP endpoints
- **Order Book** (`include/project/order_book.hpp`): In-memory order book with per-level queues, supporting Add/Cancel/Modify/Clear operations
- **RingBuffer** (`include/project/ring_buffer.hpp`): Lock-free SPSC (Single Producer Single Consumer) queue with C++20 `atomic::wait/notify` for efficient thread communication
- **DatabaseWriter** (`src/database/database_writer.cpp`, `include/database/database_writer.hpp`): Writes order book snapshots to PostgreSQL
- **JSONGenerator** (`src/database/json_generator.cpp`, `include/database/json_generator.hpp`): Generates JSON from database on-demand
- **PostgresConnection** (`src/database/postgres_connection.cpp`, `include/database/postgres_connection.hpp`): PostgreSQL connection manager
- **Web UI** (`static/index.html`): Browser-based interface for file upload and statistics display

## Architecture

### Threading Model

The server uses a multi-threaded architecture to maximize performance:

```
┌─────────────────────────────────────────────────────────────┐
│                      Main Thread                            │
│  - uWebSockets event loop (WebSocket + HTTP)                │
│  - Accepts file uploads                                     │
│  - Serves static files                                      │
│  - Handles download requests                                │
└────────────────┬────────────────────────────────────────────┘
                 │
                 ├─► Processing Thread (detached)
                 │   - Reads DBN file via DbnFileStore
                 │   - Applies MBO messages to order book
                 │   - Captures snapshots after each update
                 │   - Pushes snapshots to ring buffer
                 │   - Sends completion message to client
                 │
                 └─► Database Writer Thread
                     - Pops snapshots from ring buffer
                     - Writes snapshots to PostgreSQL
                     - Continues until ring buffer is empty
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
            │ (4) Pop snapshots
            ▼
    Database Writer Thread
            │
            │ (5) Write to PostgreSQL
            ▼
    PostgreSQL Database
            │
            │ (6) User requests download via HTTP
            ▼
    JSON Generator
            │
            │ (7) Query database & generate JSON
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
4. **Completion message sent** immediately after processing (database writes continue in background)
5. **Database writer thread** independently:
   - Pops snapshots from ring buffer
   - Writes snapshots to PostgreSQL database
   - Exits when ring buffer is empty
6. **User downloads JSON** via download button in UI (queries database and generates JSON on-demand)

## Building

### Prerequisites

- C++20 compatible compiler (GCC 11+, Clang 14+, or MSVC 2019+)
- CMake 3.20+
- PostgreSQL 12+ (running and accessible)
- [uWebSockets](https://github.com/uNetworking/uWebSockets) (included in `thirdparty/`)
- [databento-cpp](https://github.com/databento/databento-cpp) (included in `thirdparty/`)
- nlohmann/json (provided by databento-cpp dependencies)

### Build Instructions

```bash
cd server
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

The executable will be created at `build/apps/websocket_server`.

## Running

### Configuration

Edit `config/config.ini` to configure server settings:

```ini
[server]
port = 9001
top_levels = 10
output_full_book = true
ring_buffer_size = 65536

[postgres]
host = localhost
port = 5432
dbname = orderbook
user = postgres
password = postgres
max_connections = 10
connection_timeout = 30
```

**Configuration Options:**
- `port`: WebSocket/HTTP server port (default: 9001)
- `top_levels`: Number of order book levels to include in snapshots (default: 10)
- `output_full_book`: Include full book details in snapshots (default: true)
- `ring_buffer_size`: Size of SPSC ring buffer, must be power of 2 (default: 65536)
- `host`: PostgreSQL server hostname (default: localhost)
- `port`: PostgreSQL server port (default: 5432)
- `dbname`: Database name (default: orderbook)
- `user`: PostgreSQL username (default: postgres)
- `password`: PostgreSQL password (default: postgres)
- `max_connections`: Maximum database connections (default: 10)
- `connection_timeout`: Connection timeout in seconds (default: 30)

### Start Server

```bash
cd server/build/apps
./websocket_server
```

Or use the convenience script:

```bash
cd server
./scripts/start.sh
```

You should see:

```
Starting WebSocket server on port 9001
Symbol: (will be extracted from DBN file)
Top Levels: 10
Output Full Book: true
Ring Buffer Size: 65536
JSON output: ../../data/order_book_output.json
JSON writer initialized: ../../data/order_book_output.json
Server started, running_ = true
Starting JSON generation thread...
JSON generation thread started
WebSocket server listening on port 9001
HTTP server available at http://localhost:9001
```

### Using the Web Interface

1. **Open browser** to `http://localhost:9001`
2. **Select DBN file**: Click "Choose File" or drag-and-drop a `.dbn` file
3. **Upload & Process**: Click "Upload & Process" button
4. **Monitor progress**: Watch real-time statistics as file is processed
5. **View results**: See detailed statistics when processing completes
6. **Download JSON**: Click "📥 Download JSON Output" to download JSON generated from database

### HTTP Endpoints

- `GET /` - Serves web interface (index.html)
- `GET /download/json?session_id=<id>` - Downloads JSON generated from database (optional session_id parameter)
- `WS /` - WebSocket endpoint for file upload and processing

## Implementation Details

### Thread Synchronization

**Order Book Processing:**
- Single-threaded processing per upload (no locks needed)
- Each upload spawns its own processing thread
- Order book updates are sequential and atomic

**Ring Buffer Communication:**
- SPSC lock-free queue using C++20 atomics
- Producer (processing thread) pushes snapshots
- Consumer (database writer thread) pops snapshots
- Blocking operations use `atomic::wait/notify`
- No data loss: blocking push waits if buffer is full

**Database Writing:**
- Runs continuously in background thread
- Processes snapshots as they arrive
- Writes to PostgreSQL using prepared statements
- Exits cleanly when `running_` is false AND buffer is empty

**JSON Generation:**
- On-demand generation from database queries
- Queries snapshots by session_id or symbol
- Generates newline-delimited JSON format
- Served via HTTP endpoint for download

### Performance Optimizations

1. **Lock-free ring buffer**: Zero-copy snapshot passing between threads
2. **Prepared statements**: PostgreSQL prepared statements for efficient database writes
3. **Separate database writer thread**: Order book processing never blocks on I/O
4. **Pre-allocated snapshots**: Minimal heap allocations during hot path
5. **Cache-line alignment**: Ring buffer indices on separate cache lines
6. **On-demand JSON generation**: JSON only generated when requested, not during processing

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

### Statistics Reported

After processing completes, the server reports:

**Processing Metrics:**
- Messages Received: Total MBO messages processed
- Orders Processed: Successfully applied orders
- Message Throughput: Messages per second
- Average Order Process Time: Mean latency in nanoseconds
- P99 Order Process Time: 99th percentile latency

**Order Book Summary:**
- Active Orders: Total orders in book after processing
- Bid/Ask Price Levels: Number of distinct price levels
- Best Bid/Ask: Current BBO with size and order count
- Bid-Ask Spread: Current spread

### Error Handling

The server handles various error conditions gracefully:

- **Missing order references**: Silently skipped (out-of-sequence data)
- **File I/O errors**: Logged and reported to client
- **Invalid DBN format**: Error message sent via WebSocket
- **WebSocket disconnection**: Processing thread cleans up temp files
- **JSON write failures**: Logged but doesn't stop processing

### Memory Management

- **Order Book**: Dynamic allocation for price levels and orders
- **Ring Buffer**: Fixed-size pre-allocated buffer (power-of-2 size)
- **Temp Files**: Written to system temp directory, cleaned up after use
- **JSON Writer**: Uses buffered I/O, flushes periodically
- **Thread Safety**: No shared mutable state between threads

## Troubleshooting

### Server won't start

**Check port availability:**
```bash
lsof -i :9001
```
If port is in use, either kill the process or change the port in `config/config.ini`.

### Browser can't connect

- Verify server is running: `ps aux | grep websocket_server_main`
- Check firewall rules allow connections on configured port
- Ensure you're accessing `http://localhost:PORT` not `https://`

### JSON file is empty or incomplete

- Check server logs for "JSON writer initialized" message
- Verify JSON generation thread started: "JSON generation thread started"
- Ensure proper shutdown to flush remaining data
- Check disk space and write permissions

### Processing seems slow

- Verify file is valid DBN format
- Check available memory (order book can be large)
- Monitor CPU usage (processing is CPU-bound)
- Increase `ring_buffer_size` if buffer fills up


## Performance Benchmarks

Typical performance on modern hardware (Apple M1/M2, Intel i7/i9):

- **Order book throughput**: 1-2 million messages/second
- **Order processing latency**: 100-200 ns average, 500-1000 ns P99
- **JSON generation rate**: 50,000-100,000 records/second
- **Memory usage**: ~100-500 MB (depends on order book size)
- **Ring buffer latency**: Sub-microsecond producer-consumer handoff