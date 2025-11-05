## Overview

This `src` module implements a high-performance, reproducible TCP sender/receiver pipeline for replaying Databento MBO (Market-By-Order) data into an in-memory order book, generating JSON snapshots (always on), and reporting timing metrics.

### Key Components
- **TCPSender (`src/tcp_sender.cpp`, `include/tcp_sender.hpp`)**: Parses DBN records via `DbnFileStore`, batches them into compact `MboMessage` frames, and streams with `writev`. Uses C++20 `std::jthread` and exposes `getSentMessages()`, `getThroughput()`, and `getStreamingMs()`; printing is handled only by `sender_main.cpp`.
- **TCPReceiver (`src/tcp_receiver.cpp`, `include/tcp_receiver.hpp`)**: Accepts the stream, reconstructs `MboMsg`, applies it to the in-memory order book, decouples JSON generation via a blocking SPSC ring buffer, and tracks performance metrics. Uses C++20 `std::jthread` and `std::stop_token` for structured concurrency. JSON generation is always enabled and persisted to a file kept open for append.
- **Order Book (`include/order_book.hpp`)**: Reference book with per-level queues. Supports `Add/Cancel/Modify/Clear`, BBO lookup, snapshots, and optional JSON snapshot generation hooks.
- **RingBuffer (`include/ring_buffer.hpp`)**: Lock-free single-producer single-consumer queue with C++20 `atomic::wait/notify`; provides blocking `push/pop` to decouple the hot path from JSON generation without drops.
- **Streamer (offline helper) (`src/streamer.cpp`, `include/streamer.hpp`)**: Convenience loader/iterator for DBN records (not used in TCP pipeline runtime).
- **Config (`include/config.hpp`, `src/config.cpp`, `config.ini`)**: Simple key-value configuration for ports, batching, symbols, and feature toggles.

## Architecture

### Data Flow
1. Sender loads DBN and pre-parses all `MboMsg` records into memory for deterministic replay.
2. Sender batches messages (size configurable) into a packed `MboMessage` and transmits via TCP using `writev`.
3. Receiver reads socket bytes into a large circular buffer, processes as many complete messages as available, and avoids extra copies/memmove entirely.
4. Receiver converts `MboMessage` back to `databento::MboMsg` and applies to `Book` under a write lock.
5. The receiver pushes a lightweight wrapper to a blocking SPSC ring buffer consumed by a dedicated JSON thread (no drops, preserves exact order).
6. JSON thread reads the book under a shared lock to build snapshots, buffers lines, and flushes to disk in batches to a persistently-open file.

### Threading Model
- **Sender**: Single background thread `streamingThread_` (`std::jthread`) handles accept, handshake, parsing, batching, and network writes with `stop_token`. Client socket sets `TCP_NODELAY` and large `SO_SNDBUF`.
- **Receiver**:
  - `receivingThread_` (`std::jthread`): network read → parse → order book `Apply` (write lock) → push JSON work to ring buffer (blocking `push`).
  - `jsonGenerationThread_` (`std::jthread`): blocking `pop` from ring buffer → compute snapshot under shared lock → append to in-memory JSON batch → periodic/batch flush to file.

Locks and synchronization:
- `orderBookMutex_` (shared_mutex): write-locked for `Apply`, read-locked for JSON snapshot reads.
- SPSC ring buffer between threads avoids locking; producer/consumer indices are cacheline-aligned and use `atomic::wait/notify` for efficient blocking. Receiver network path uses a circular byte buffer to avoid extra copies.

### Networking Protocol
- Client/server over TCP on localhost by default.
- Sender waits for a connection, then expects a `START_STREAMING` ASCII signal from receiver.
- Messages are sent as packed `MboMessage` (fixed-size, packed, little-endian) in batches via `writev`.
- Receiver reads into a 128 KiB buffer, parses fixed-size frames, and handles partial reads.

### Performance Techniques
- Batch I/O with `writev` (sender) and batched decode with single `memmove` per read (receiver).
- Large socket buffers and `TCP_NODELAY` to reduce latency.
- Blocking SPSC ring buffer (no drops) to decouple JSON generation from the hot path while preserving exact processing order. Persistent JSON file handle to reduce open/close overhead.
- Cacheline alignment for ring buffer indices to avoid false sharing.
- JSON batching with size and interval thresholds to reduce syscalls; `std::ofstream` append+flush used on batch boundaries.
- Compiler flags `-O3 -DNDEBUG -march=native` and link-time optimization enabled.

## Metrics

- **Receiver metrics**
  - `Messages Received`: count of frames read from the socket.
  - `Orders Successfully Processed`: count of `Book::Apply` operations that completed.
  - `Message Throughput`: messages per second from first processed message to end of processing.
  - `Average Order Processing Time (ns)`: average wall-clock time inside the `Apply` critical section per message.
  - `P99 Order Processing Time (ns)`: 99th percentile of per-apply wall-clock times (via nth_element).
  - `JSON Records Generated`: lines successfully generated by the JSON thread (may be less than messages if the ring buffer overflowed by design).

- **Sender metrics**
  - `Messages Sent`: total frames transmitted.
  - `Throughput`: messages per second from streaming start to completion.

Notes:
- Apply timings measure book modification cost, not network or JSON costs.
- P99 is computed on a copy of the recorded times using `nth_element` to avoid sorting overhead.

## Configuration

Defaults are provided in `config.ini`. Environment variable `ASSIGNMENT_CONFIG` can override the path.

Important keys:
- `sender.port`: TCP port for the sender (default 8080)
- `sender.batch_size`: number of frames per `writev` batch
- `receiver.host`: receiver target host (default 127.0.0.1)
- `receiver.port`: receiver target port
- `receiver.symbol`: symbol string to tag in JSON
- `receiver.top_levels`: number of levels to include in JSON snapshots
- `receiver.json_output_file`: output path for JSON lines
- `receiver.json_batch_size`: number of JSON lines per flush
- `receiver.json_flush_interval`: modulo-based periodic flush interval

## Build and Run

### Prerequisites
- CMake 3.16+
- C++20 toolchain
- Databento C++ SDK built under `../databento-cpp`

### Build
From `src/`:

```bash
./start.sh
```

This script configures and builds both `TCP_Sender` and `TCP_Receiver` in `src/build/`, then runs the pipeline.

### Manual run
```bash
cd src/build
./TCP_Sender &
./TCP_Receiver
```

## Operational Notes

- The receiver disables in-book JSON generation (`Book::enableJsonOutput(false)`) and performs snapshots in a separate thread for stability.
- On shutdown, structured cancellation (`std::stop_token`) is used; sockets are shut down to unblock reads, the JSON thread drains remaining items, and final JSON flushes are forced.
- JSON ordering exactly matches processing order; the ring buffer is blocking so records are not dropped.
- Sender stats are printed only from `sender_main.cpp` (not from `tcp_sender.cpp`). `sender_main` prints streaming time, messages sent, and throughput using the sender’s getters.

## Extensibility

- Replace TCP framing with a different transport by adapting `MboMessage` marshalling in sender/receiver.
- Introduce additional consumers by adding new SPSC queues from the receiving thread.
- Extend metrics by recording more timestamps or using histograms.

## Troubleshooting

- If build fails to find Databento headers/libraries, verify paths in `src/CMakeLists.txt` include directories and static lib location.
- If receiver prints 0 throughput, ensure a connection was established and `START_STREAMING` was sent.
- To reduce disk I/O, disable JSON or increase `receiver.json_batch_size`.


