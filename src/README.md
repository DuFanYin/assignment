## Overview

This `src` module implements a high-performance, reproducible TCP sender/receiver pipeline for replaying Databento MBO (Market-By-Order) data into an in-memory order book, generating JSON snapshots (always on), and reporting timing metrics.

### Key Components
- **TCPSender (`src/src/tcp_sender.cpp`, `include/project/tcp_sender.hpp`)**: Parses DBN records via `DbnFileStore`, batches them into compact `MboMessage` frames, and streams with `writev`. Uses C++20 `std::jthread` and exposes `getSentMessages()`, `getThroughput()`, `getStreamingMs()`, and `getStreamingUs()`. Printing is handled only by `apps/sender_main.cpp`.
- **TCPReceiver (`src/src/tcp_receiver.cpp`, `include/project/tcp_receiver.hpp`)**: Accepts the stream, reconstructs `MboMsg`, applies it to the in-memory order book, decouples JSON generation via a blocking SPSC ring buffer, and tracks performance metrics. Uses C++20 `std::jthread` and `std::stop_token` for structured concurrency. JSON generation is always enabled and persisted to a file kept open for append.
- **Order Book (`include/project/order_book.hpp`)**: Reference book with per-level queues. Supports `Add/Cancel/Modify/Clear`, BBO lookup, snapshots, and JSON snapshot generation hooks.
- **RingBuffer (`include/project/ring_buffer.hpp`)**: Lock-free single-producer single-consumer queue with C++20 `atomic::wait/notify`; provides blocking `push/pop` to decouple the hot path from JSON generation without drops.
- **Config (`src/src/config.cpp`, `include/project/config.hpp`, `config/config.ini`)**: Simple key-value configuration for ports, batching, symbols, and JSON settings. All configuration paths are required (no fallbacks).

## Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Sender Application                          │
│                    (1 thread: streamingThread_)                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  DBN File → DbnFileStore → Parse & Batch → writev() → TCP Socket   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ TCP (localhost:8080)
                                    │ Packed MboMessage frames
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        Receiver Application                         │
│              (2 threads: receivingThread_, jsonGenerationThread_)   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────────────────┐     ┌──────────────────────────┐   │
│  │  Thread 1: Receiving     │     │  Thread 2: JSON Gen      │   │
│  │  ──────────────────────  │     │  ──────────────────────  │   │
│  │                          │     │                          │   │
│  │  TCP Socket → Circular   │     │  Ring Buffer → Pop       │   │
│  │  Buffer (128 KiB)        │     │  MboMessageWrapper       │   │
│  │                          │     │                          │   │
│  │  Parse MboMessage →      │     │  Generate JSON → Batch   │   │
│  │  Apply to Order Book     │────▶│  Buffer → Flush to File  │   │
│  │  (write lock)            │     │  (shared lock)           │   │
│  │                          │     │                          │   │
│  │  Push to Ring Buffer     │     │                          │   │
│  │  (blocking)              │     │                          │   │
│  └──────────────────────────┘     └──────────────────────────┘   │
│            │                                  │                   │
│            └──────────┬───────────────────────┘                   │
│                       │                                           │
│                   Order Book                                      │
│              (shared_mutex protected)                              │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Flow
1. Sender loads DBN file via `DbnFileStore` and streams records in real-time.
2. Sender batches messages (size configurable) into packed `MboMessage` frames and transmits via TCP using `writev()`.
3. Receiver reads socket bytes into a 128 KiB circular buffer, processes complete messages without `memmove` overhead.
4. Receiver converts `MboMessage` back to `databento::MboMsg` and applies to `Book` under a write lock.
5. The receiving thread pushes a lightweight wrapper to a blocking SPSC ring buffer (no drops, preserves exact order).
6. JSON generation thread pops from ring buffer, reads the book under a shared lock to build snapshots, buffers lines, and flushes to disk in batches to a persistently-open file.

### Threading Model
- **Sender Application**: 1 thread
  - `streamingThread_` (`std::jthread`): Handles server accept, client connection, handshake, parsing DBN records, batching, and network writes with `stop_token`. Client socket sets `TCP_NODELAY` and large `SO_SNDBUF` (32 MiB).
  
- **Receiver Application**: 2 threads
  - `receivingThread_` (`std::jthread`): Network read → parse → order book `Apply` (write lock) → push JSON work to ring buffer (blocking `push`).
  - `jsonGenerationThread_` (`std::jthread`): Blocking `pop` from ring buffer → compute snapshot under shared lock → append to in-memory JSON batch → periodic/batch flush to file.

**Locks and synchronization:**
- `orderBookMutex_` (shared_mutex): Write-locked for `Apply`, read-locked for JSON snapshot reads.
- SPSC ring buffer between threads avoids locking; producer/consumer indices are cacheline-aligned and use `atomic::wait/notify` for efficient blocking.
- Receiver network path uses a circular byte buffer (`std::array`, `std::span`) to avoid extra copies and eliminate `memmove`.

### Networking Protocol
- Client/server over TCP on localhost by default.
- Sender waits for a connection, then expects a `START_STREAMING` ASCII signal from receiver.
- Messages are sent as packed `MboMessage` (fixed-size, packed, little-endian) in batches via `writev`.
- Receiver reads into a 128 KiB buffer, parses fixed-size frames, and handles partial reads.

### Performance Techniques
- Batch I/O with `writev` (sender) for efficient multi-message transmission.
- Circular buffer (`std::array`, `std::span`) in receiver eliminates `memmove` overhead entirely.
- Large socket buffers (32 MiB send buffer) and `TCP_NODELAY` to reduce latency.
- Blocking SPSC ring buffer (no drops) to decouple JSON generation from the hot path while preserving exact processing order. Persistent JSON file handle to reduce open/close overhead.
- Cacheline alignment for ring buffer indices to avoid false sharing.
- JSON batching with size and interval thresholds to reduce syscalls; `std::ofstream` append+flush used on batch boundaries.
- Compiler flags `-O3 -DNDEBUG -march=native` and link-time optimization enabled.
- C++20 features: `std::jthread`, `std::stop_token`, `std::atomic::wait/notify`, `std::format`, `std::span`, `std::ranges`, `constexpr` helpers, and concepts.

## Metrics

- **Receiver metrics** (printed by `apps/receiver_main.cpp`)
  - `Messages Received`: count of frames read from the socket.
  - `Orders Successfully Processed`: count of `Book::Apply` operations that completed.
  - `JSON Records Generated`: lines successfully generated by the JSON thread (matches processing order exactly).
  - `Message Throughput`: messages per second from first processed message to end of processing.
  - `Average Order Processing Time (ns)`: average wall-clock time inside the `Apply` critical section per message.
  - `P99 Order Processing Time (ns)`: 99th percentile of per-apply wall-clock times (via nth_element).
  - `Order Processing Rate`: orders per second calculated from average processing time.

- **Sender metrics** (printed by `apps/sender_main.cpp`)
  - `Total Messages Sent`: total frames transmitted.
  - `Average Throughput`: messages per second from streaming start to completion.
  - `Streaming Time`: total duration in microseconds (μs) or milliseconds (ms).

**Notes:**
- Apply timings measure book modification cost, not network or JSON costs.
- P99 is computed on a copy of the recorded times using `nth_element` to avoid sorting overhead.
- JSON records maintain exact ordering with processed orders (blocking ring buffer ensures no drops).
- All metrics are aligned in columns for readability.

## Configuration

Configuration is provided in `config/config.ini`. The `ASSIGNMENT_CONFIG` environment variable must be set to the config file path (e.g., `../config/config.ini`). All configuration keys are required; no fallback defaults are provided.

**Required configuration keys:**

- **Sender:**
  - `sender.port`: TCP port for the sender (e.g., 8080)
  - `sender.batch_size`: number of frames per `writev` batch
  - `sender.data_file`: path to DBN file (relative to build directory, e.g., `../data/CLX5_mbo.dbn`)

- **Receiver:**
  - `receiver.host`: receiver target host (e.g., 127.0.0.1)
  - `receiver.port`: receiver target port (must match sender.port)
  - `receiver.symbol`: symbol string to tag in JSON (e.g., CLX5)
  - `receiver.top_levels`: number of levels to include in JSON snapshots
  - `receiver.output_full_book`: whether to output full book stats (true/false)
  - `receiver.json_output_file`: output path for JSON lines (relative to build directory, e.g., `../data/order_book_output.json`)
  - `receiver.json_batch_size`: number of JSON lines per flush
  - `receiver.json_flush_interval`: modulo-based periodic flush interval

## Build and Run

### Prerequisites
- CMake 3.16+
- C++20 toolchain
- Databento C++ SDK built under `../../databento-cpp` (relative to project root)

### Build

The `scripts/start.sh` script supports two modes:

**Build mode** (build only):
```bash
cd src
./scripts/start.sh build
```

This configures CMake and compiles the project. Executables are generated in `build/apps/`:
- `build/apps/sender`
- `build/apps/receiver`

**Run mode** (execute pipeline):
```bash
cd src
./scripts/start.sh run
# or simply:
./scripts/start.sh
```

This runs the pipeline: starts the sender in the background, then starts the receiver. The script automatically:
- Cleans up any existing JSON output file
- Kills existing processes on port 8080
- Sets `ASSIGNMENT_CONFIG` environment variable
- Starts sender and receiver
- Displays final output file summary


## Operational Notes

- JSON generation is always enabled and handled by a dedicated thread in the receiver application (`jsonGenerationThread_`). The receiver performs JSON snapshots in a separate thread for stability, decoupled from the hot path via a blocking ring buffer.
- On shutdown, structured cancellation (`std::stop_token`) is used: sockets are shut down to unblock reads, the JSON thread drains remaining items from the ring buffer, and final JSON flushes are forced.
- JSON ordering exactly matches processing order; the blocking ring buffer ensures records are not dropped.
- Sender stats are printed only from `apps/sender_main.cpp` (not from `tcp_sender.cpp`). `sender_main` prints streaming time (in μs or ms), messages sent, and throughput using the sender's getters.
- Receiver stats are printed only from `apps/receiver_main.cpp` with aligned column formatting.
- The sender application uses 1 thread (`streamingThread_`). The receiver application uses 2 threads (`receivingThread_` and `jsonGenerationThread_`).


## Troubleshooting

- If build fails to find Databento headers/libraries, verify paths in `src/src/CMakeLists.txt` include directories and static lib location relative to the project root.
- If receiver prints 0 throughput, ensure a connection was established and `START_STREAMING` was sent.
- If executables are not found when running, ensure you built the project first with `./scripts/start.sh build`.
- If configuration errors occur, verify `ASSIGNMENT_CONFIG` environment variable is set and all required keys are present in the config file.
- To reduce disk I/O, increase `receiver.json_batch_size` and `receiver.json_flush_interval` (JSON generation cannot be disabled).


