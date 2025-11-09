## Overview

This `src` module implements a high-performance, reproducible TCP sender/receiver pipeline for replaying Databento MBO (Market-By-Order) data into an in-memory order book, generating JSON snapshots (always on), and reporting timing metrics. The receiver now captures an immutable snapshot immediately after each successful apply and generates JSON from that snapshot, fully decoupling JSON from the live book and eliminating read-side locking.

### Key Components
- **TCPSender** (`src/src/tcp_sender.cpp`, `include/project/tcp_sender.hpp`): Parses DBN records via `DbnFileStore`, batches them into compact `MboMessage` frames, and streams with `writev`. Uses C++20 `std::jthread` and exposes `getSentMessages()`, `getThroughput()`, `getStreamingMs()`, and `getStreamingUs()`. Printing is handled only by `apps/sender_main.cpp`.
- **TCPReceiver** (`src/src/tcp_receiver.cpp`, `include/project/tcp_receiver.hpp`): Accepts the stream, reconstructs `MboMsg`, applies it to the in-memory order book, captures a minimal post-apply snapshot, enqueues it to a blocking SPSC ring buffer, and tracks performance metrics. Uses C++20 `std::jthread` and `std::stop_token` for structured concurrency. JSON generation is always enabled and persisted via a portable writer that keeps the file open for append.
- **Order Book** (`include/project/order_book.hpp`): Reference book with per-level queues. Supports `Add/Cancel/Modify/Clear`, BBO lookup, and count queries. With the snapshot design, the live book is updated only by the receiving thread during processing; JSON reads no longer touch the book.
- **RingBuffer** (`include/project/ring_buffer.hpp`): Lock-free single-producer single-consumer queue with C++20 `atomic::wait/notify`; provides blocking `push/pop` to decouple the hot path from JSON generation without drops.
- **Config** (`src/src/config.cpp`, `include/project/config.hpp`, `config/config.ini`): Simple key-value configuration for ports, batching, symbols, and JSON settings. All configuration paths are required (no fallbacks).

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
│  │  + Capture Snapshot      │     │  (from snapshot only)    │   │
│  │                          │     │                          │   │
│  │  Push to Ring Buffer     │     │                          │   │
│  │  (blocking)              │     │                          │   │
│  └──────────────────────────┘     └──────────────────────────┘   │
│            │                                  │                   │
│            └──────────┬───────────────────────┘                   │
│                       │                                           │
│                   Order Book                                      │
│             (platform-optimized lock protected)                    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Flow

1. Sender loads DBN file via `DbnFileStore` and streams records in real-time.
2. Sender batches messages (size configurable) into packed `MboMessage` frames and transmits via TCP using `writev()`.
3. Receiver reads socket bytes into a 128 KiB circular buffer, processes complete messages without `memmove` overhead.
4. Receiver converts `MboMessage` back to `databento::MboMsg` and applies to `Book` (single-threaded, no lock needed).
5. Immediately after apply, the receiving thread captures a minimal `BookSnapshot` (BBO, top-N levels, counts, timestamp, symbol) and pushes it to a blocking SPSC ring buffer (no drops, preserves exact order).
6. JSON generation thread pops snapshots, builds JSON strictly from the snapshot (never reading the live book), buffers lines, and flushes to disk in batches to a persistently-open file.

### Threading Model and Concurrency

**Critical Design Principle: All processing happens in worker threads; main threads are minimal and non-blocking.**

**Sender Application (process):**
- **Main thread** (in `apps/sender_main.cpp`): 
  - Loads configuration from file
  - Creates `TCPSender` instance and configures it
  - Calls `startStreaming()` to launch worker thread
  - **Waits passively** in a loop checking `isStreaming()` status
  - Prints summary statistics after completion
  - **Net: Minimal work, essentially just setup and monitoring**
- **Worker thread**: `streamingThread_` (`std::jthread`) inside `TCPSender` performs **ALL actual work**:
  - Server socket setup and client connection acceptance
  - DBN file reading via `DbnFileStore`
  - Message pre-parsing into memory (`allMessages` vector)
  - Batching into `MboMessage` frames
  - TCP transmission via `writev()` (with `std::stop_token` for cancellation)
  - **Net: 1 process, 1 worker thread doing all processing**
  
**Receiver Application (process):**
- **Main thread** (in `apps/receiver_main.cpp`): 
  - Loads configuration from file
  - Creates `Book` and `TCPReceiver` instances
  - Establishes TCP connection via `connect()`
  - Calls `startReceiving()` to launch worker threads
  - **Waits passively** in a loop checking `isConnected()` status
  - Prints summary statistics after completion
  - **Net: Minimal work, essentially just setup and monitoring**
- **Worker thread 1**: `receivingThread_` (`std::jthread`) performs **ALL receiving and order book processing**:
  - Socket reads into circular buffer (128 KiB)
  - Message parsing from raw bytes
  - Conversion to `databento::MboMsg`
  - Order book updates via `Book::Apply()` (single-threaded, no lock)
  - Capture `BookSnapshot` and push to ring buffer for JSON generation
- **Worker thread 2**: `jsonGenerationThread_` (`std::jthread`) performs **ALL JSON generation**:
  - Pop from ring buffer (blocking, preserves order)
  - Serialize snapshot (no access to live book)
  - JSON string generation
  - Batching and flushing to file
  - **Net: 1 process, 2 worker threads doing all processing**

**Synchronization Mechanisms:**
- Live order book is updated only by the receiving thread during processing; no locks are required on `Apply` or snapshot capture.
- JSON thread never touches the live book; it serializes from the immutable `BookSnapshot` only.
- SPSC ring buffer between threads avoids locking; producer/consumer indices are cacheline-aligned and use `atomic::wait/notify` for efficient blocking.
- Receiver network path uses a circular byte buffer (`std::array`, `std::span`) to avoid extra copies and eliminate `memmove`.

**Why This Design is Beneficial and Ideal:**

1. **Non-blocking Main Thread**: The main thread never blocks on I/O or heavy computation. It remains responsive, can handle signals, and can perform cleanup gracefully. This is crucial for production systems where the main thread may need to respond to shutdown signals or other events.

2. **Separation of Concerns**: Each worker thread has a single, well-defined responsibility:
   - Sender: File I/O → Network I/O (streaming)
   - Receiver Thread 1: Network I/O → Order Book Processing
   - Receiver Thread 2: JSON Generation → File I/O
   This makes the code easier to reason about, debug, and optimize.

3. **Parallel Processing**: On the receiver side, order book processing and JSON generation happen in parallel. The receiving thread can continue processing new messages while the JSON thread generates snapshots asynchronously, maximizing throughput.

4. **Producer-Consumer Decoupling**: The SPSC ring buffer decouples the hot path (receiving/processing) from the slower path (JSON generation/file I/O). If JSON generation slows down (e.g., disk I/O), it doesn't block message processing. The blocking ring buffer ensures no data loss while providing natural backpressure.

5. **Structured Concurrency**: Using C++20 `std::jthread` and `std::stop_token` ensures proper thread lifecycle management. Threads can be cancelled cleanly, and the main thread can wait for completion without manual thread management.

6. **Better Resource Utilization**: By dedicating threads to different tasks, we can leverage multiple CPU cores effectively. The receiver can process messages on one core while generating JSON on another.

7. **Isolation of Failure Domains**: If one worker thread encounters an error, it doesn't immediately crash the entire process. The main thread can detect failures and perform cleanup.

8. **Scalability**: This design makes it straightforward to add more worker threads in the future (e.g., multiple JSON generators) without changing the main thread logic.

### Order Preservation and Loss Prevention Guarantees

✅ **No order loss, no out-of-order processing** - The design guarantees strict FIFO ordering at every stage:

**Network → ReceivingThread (TCP to Processing):**
- TCP protocol guarantees in-order delivery (reliable stream)
- ReceivingThread reads sequentially from socket (`read()` syscall)
- Circular buffer processes messages in strict FIFO order (head-to-tail)
- Each message is applied to Book sequentially (`Apply()` called in order)
- **Guarantee**: Messages are processed in the exact order received from TCP

**ReceivingThread → RingBuffer → JSONThread (Processing to JSON Generation):**
- ReceivingThread pushes to RingBuffer using **blocking `push()`**
- Blocking push means: if buffer is full, it **waits** rather than dropping (no data loss)
- RingBuffer is **SPSC (Single Producer Single Consumer)** - only one producer (ReceivingThread) and one consumer (JSONThread)
- SPSC queue with atomic operations guarantees **strict FIFO ordering** (FIFO = First In First Out)
- JSONThread pops sequentially using `try_pop()` + `wait_for_data()` - ensures all items are processed in order
- **Guarantee**: JSON records are generated in the exact same order as messages were processed

**Why no drops occur:**
- RingBuffer uses **blocking push** (not `try_push`) - if full, producer waits for space
- RingBuffer uses **blocking pop** (via `wait_for_data()`) - consumer waits for data
- RingBuffer capacity (65536) is large enough to handle bursts
- Comment in code explicitly states: "Blocking push to preserve order and avoid drops" (line 263)

**Why no out-of-order occurs:**
- Single-threaded processing in ReceivingThread (no parallelization)
- SPSC queue guarantees FIFO ordering (only one producer, one consumer)
- Sequential socket reads and sequential Book updates
- Atomic operations with proper memory ordering (`memory_order_release` on write, `memory_order_acquire` on read)

### Networking Protocol

- Client/server over TCP on localhost by default.
- Sender waits for a connection, then expects a `START_STREAMING` ASCII signal from receiver.
- Messages are sent as packed `MboMessage` (fixed-size, packed, little-endian) in batches via `writev`.
- Receiver reads into a 128 KiB buffer, parses fixed-size frames, and handles partial reads.

### Performance Techniques

- Batch I/O with `writev` (sender) for efficient multi-message transmission.
- Circular buffer (`std::array`, `std::span`) in receiver eliminates `memmove` overhead entirely.
- Large socket buffers (32 MiB send buffer) and `TCP_NODELAY` to reduce latency.
- Blocking SPSC ring buffer (no drops) to decouple JSON generation from the hot path while preserving exact processing order. Persistent JSON handle to reduce open/close overhead.
- Cacheline alignment for ring buffer indices to avoid false sharing.
- JSON batching with size and interval thresholds to reduce syscalls; on macOS/Linux, JSON is written via a memory-mapped file with batched `msync(MS_ASYNC)`; on other platforms, a buffered `std::ofstream` append fallback is used.
- Thread affinity on macOS Apple Silicon: worker threads are optionally pinned using `THREAD_AFFINITY_POLICY` via a small helper (`include/project/thread_affinity.hpp`). Calls are no-ops on non-macOS.
- Compiler flags `-O3 -DNDEBUG -march=native` and link-time optimization enabled.
- C++20 features: `std::jthread`, `std::stop_token`, `std::atomic::wait/notify`, `std::format`, `std::span`, `std::ranges`, `constexpr` helpers, and concepts.

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

### Platform-specific behavior

- macOS (Darwin):
  - Worker threads are pinned to cores where applicable (sender: core 0; receiver: receiving on core 1, JSON generation on core 0).
  - Order book lock uses `os_unfair_lock` via RAII guards for lower overhead.
  - JSON writer uses `mmap` with `msync(MS_ASYNC)` on batch boundaries; file remains open for the entire runtime and is truncated to the final size on shutdown.
- Linux:
  - JSON writer also uses `mmap` + `msync(MS_ASYNC)`.
- Other platforms (e.g., Windows):
  - JSON writer falls back to buffered `std::ofstream` append + batched flush.

## Build and Run

### Prerequisites
- CMake 3.16+
- C++20 toolchain
- Databento C++ SDK built under `../../databento-cpp` (relative to project root)

Platform detection:
- CMake detects macOS via `CMAKE_SYSTEM_NAME STREQUAL "Darwin"` and defines `PLATFORM_DARWIN=1` used by platform-specific implementations.

### Build Mode
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

### Implementation notes (where to look)

- Thread affinity helper:
  - `include/project/thread_affinity.hpp`, `src/src/thread_affinity.cpp`
- JSON writer (mmap on macOS/Linux, ofstream fallback elsewhere):
  - `include/project/json_writer.hpp`, `src/src/json_writer.cpp`
- Order book implementation:
  - `include/project/order_book.hpp` (no runtime locks; single-threaded updates during processing)

## Metrics and Operational Notes

### Metrics

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

### Operational Notes

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
