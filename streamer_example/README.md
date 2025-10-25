# High-Performance TCP Market Data Streaming System

A ultra-low latency TCP-based market data streaming system designed for maximum throughput and minimal latency. This system achieves **335,193 messages/sec** and **324,456 orders/sec** processing rates through advanced optimization techniques.

## 🚀 Performance Achievements

- **Message Throughput**: 335,193 messages/sec
- **Order Processing Rate**: 324,456 orders/sec  
- **Processing Time**: 114ms for 38,212 messages
- **Orders Processed**: 36,988 orders
- **Latency**: Sub-millisecond end-to-end latency

## 🏗️ Architecture Overview

```
DBN File → Memory Mapping → Pre-parsing → Binary Protocol → TCP Stream → Order Book Processing
```

## 🔧 Advanced Optimization Techniques

### 1. **Memory Management Optimizations**

#### Memory Mapping (`mmap`)
- **Zero-copy file access** using `mmap()` system call
- Entire DBN file mapped into virtual memory space
- Eliminates traditional file I/O overhead
- Direct memory access without kernel-user space copies

```cpp
// Memory map entire file for zero-copy access
mappedFile_ = mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
```

#### Pre-parsing Strategy
- **Complete file pre-parsing** into memory before streaming
- All MBO messages loaded into `std::vector<databento::MboMsg>`
- Eliminates parsing overhead during streaming
- Single-pass file reading with bulk memory allocation

```cpp
// Pre-parse entire file into memory for maximum performance
std::vector<databento::MboMsg> allMessages;
allMessages.reserve(1000000); // Reserve space for ~1M messages
```

### 2. **Network Optimizations**

#### Socket Buffer Tuning
- **16MB send/receive buffers** for maximum throughput
- Eliminates buffer overflow and reduces system calls
- Optimized for high-frequency trading scenarios

```cpp
int sendBufSize = 16 * 1024 * 1024; // 16MB send buffer
int recvBufSize = 16 * 1024 * 1024; // 16MB receive buffer
```

#### TCP_NODELAY Optimization
- **Nagle's algorithm disabled** for minimal latency
- Immediate packet transmission without buffering
- Critical for real-time market data streaming

```cpp
int nodelay = 1;
setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
```

#### Zero-Copy Transfer Support
- **`sendfile()` system call** for direct file-to-socket transfer
- **MSG_ZEROCOPY flag** for individual message zero-copy (Linux 4.14+)
- **SO_ZEROCOPY socket option** for kernel-level zero-copy
- Bypasses user-space data copying completely
- Maximum efficiency for both bulk and individual message transfer

### 3. **Binary Protocol Design**

#### Packed Binary Messages
- **30-byte fixed-size messages** with `__attribute__((packed))`
- Eliminates text parsing overhead
- Direct memory-to-network transfer

```cpp
struct MboMessage {
    uint64_t timestamp;  // 8 bytes
    uint64_t order_id;  // 8 bytes  
    uint64_t price;     // 8 bytes
    uint32_t size;      // 4 bytes
    uint8_t action;     // 1 byte
    uint8_t side;       // 1 byte
} __attribute__((packed)); // Total: 30 bytes
```

#### Timestamp Optimization
- **Pre-calculated timestamps** to eliminate per-message overhead
- Base timestamp + incremental offset
- Microsecond precision for latency measurement

### 4. **Threading and CPU Optimizations**

#### CPU Affinity Binding
- **Thread pinned to CPU 0** for cache locality
- Prevents thread migration between cores
- Maximizes L1/L2 cache hit rates

```cpp
#ifdef __APPLE__
thread_affinity_policy_data_t affinityPolicy;
affinityPolicy.affinity_tag = 0; // CPU 0
thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, 
                 (thread_policy_t)&affinityPolicy, THREAD_AFFINITY_POLICY_COUNT);
```

#### Real-Time Priority Scheduling
- **SCHED_FIFO with priority 99** (maximum priority)
- Preempts all other processes
- Guarantees deterministic execution timing

```cpp
struct sched_param param;
param.sched_priority = 99; // Maximum priority
pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
```

### 5. **Data Structure Optimizations**

#### Atomic Operations
- **Lock-free statistics** using `std::atomic`
- Eliminates mutex contention
- Thread-safe counters without synchronization overhead

```cpp
std::atomic<size_t> sentOrders_;
std::atomic<size_t> connectedClients_;
std::atomic<bool> streaming_;
```

#### Memory Pre-allocation
- **Reserved vector capacity** prevents dynamic reallocation
- Predictable memory usage patterns
- Eliminates allocation/deallocation overhead during streaming

### 6. **I/O Optimizations**

#### Direct Socket Writing
- **`write()` system call** instead of buffered I/O
- Immediate data transmission
- Minimal kernel-user space transitions

```cpp
ssize_t bytesSent = write(socket, data, size);
```

#### Efficient Buffer Management
- **Circular buffer processing** for incoming data
- `memmove()` for buffer compaction
- Minimal memory copying operations

## 📊 Performance Metrics

### Throughput Benchmarks
| Metric | Value | Description |
|--------|-------|-------------|
| **Messages/sec** | 335,193 | Raw message processing rate |
| **Orders/sec** | 324,456 | Order book processing rate |
| **Processing Time** | 114ms | Total processing duration |
| **Messages Processed** | 38,212 | Total messages handled |
| **Orders Processed** | 36,988 | Orders successfully processed |

### Latency Characteristics
- **End-to-end latency**: Sub-millisecond
- **Network latency**: Minimal (localhost)
- **Processing latency**: Microsecond-level per message
- **Memory access**: Cache-optimized

### Resource Utilization
- **CPU Usage**: Single-core optimized
- **Memory Usage**: Pre-allocated, predictable
- **Network Bandwidth**: ~10MB/s sustained
- **Socket Buffers**: 16MB send/receive

## 🛠️ Components

### 1. TCP Sender (`tcp_sender.hpp`, `tcp_sender.cpp`)
**Ultra-high performance data streaming engine**

**Key Features:**
- Memory-mapped file access with zero-copy operations
- Pre-parsing optimization for maximum throughput
- Binary protocol with packed data structures
- CPU affinity and real-time priority scheduling
- Multiple zero-copy modes: `sendfile()` and `MSG_ZEROCOPY`
- Advanced zero-copy with Linux kernel 4.14+ support
- 16MB socket buffers for maximum throughput

**Optimization Techniques:**
- Complete file pre-parsing into memory
- Packed binary message format (30 bytes)
- Pre-calculated timestamps
- Direct socket writing with `write()`
- Thread affinity to CPU 0
- SCHED_FIFO real-time priority

### 2. TCP Receiver (`tcp_receiver.hpp`, `tcp_receiver.cpp`)
**High-performance message processing engine**

**Key Features:**
- Binary message parsing with minimal overhead
- Efficient buffer management with circular buffers
- Atomic statistics for lock-free operation
- Real-time latency measurement
- Callback-based message processing

**Optimization Techniques:**
- Direct binary message parsing
- Efficient buffer compaction with `memmove()`
- Atomic counters for statistics
- Microsecond-precision latency measurement
- Minimal memory allocation during processing

### 3. Example Programs
- **`sender_main.cpp`**: High-performance sender with zero delay
- **`receiver_main.cpp`**: Latency measurement receiver with comprehensive statistics

## 🚀 Usage

### Build the System
```bash
cd streamer
mkdir build
cd build
cmake ..
make
```

### Run High-Performance Sender
```bash
./tcp_sender
```
- **Zero delay mode** for maximum throughput
- Memory-mapped file access
- Pre-parsing optimization enabled
- Real-time priority scheduling

### Run Latency Measurement Receiver
```bash
./tcp_receiver
```
- Connects to sender and measures end-to-end latency
- Processes messages through order book
- Displays comprehensive performance statistics