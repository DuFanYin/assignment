# Order Book Streamer

A high-performance market data processing system that streams Market-By-Order (MBO) data via TCP and builds a real-time order book with JSON output.

## 🚀 Three Implementation Versions

This project provides three different implementations to suit various use cases:

### performace statistic 

[src version](src/README.md) is most optimised 

| Metric                                | src                  |
|----------------------------------------|----------------------|
| **Sender: Streaming Time**             | 2.929 ms             |
| **Sender: Messages Sent**              | 38,212               |
| **Sender: TCP Throughput**             | 13,046,090.82 msg/sec|
| **Receiver: Total Throughput**         | 530,722 msg/sec      |
| **Receiver: Order Processing Rate**    | 691,999 orders/sec   |
| **Average time per order**             | 1,445 ns             |
| **P99 Order Processing Time**          | 2,250 ns             |


| Metric                                 | src        |
|-----------------------------------------|------------|
| **Receiver: Messages Received**         | 38,212     |
| **Receiver: Orders Processed**          | 36,988     |
| **Receiver: Messages Skipped**          | 1,224      |
| **Receiver: JSON Records Generated**    | 36,988     |
| **Receiver: Active Orders**             | 147        |
| **Receiver: Bid Price Levels**          | 61         |
| **Receiver: Ask Price Levels**          | 52         |
| **Receiver: Best Bid**                  | 65.40 @ 1 (1 orders)   |
| **Receiver: Best Ask**                  | 64.78 @ 3 (1 orders)   |
| **Receiver: Bid-Ask Spread**            | 0.62       |


### 1. **Standalone Version** (`/src/`)
Simple, single-machine implementation for local development and testing.

```
┌─────────────┐    TCP     ┌──────────────┐
│   Sender    │ ─────────▶ │   Receiver   │
└─────────────┘            └──────────────┘
                                  │
                                  ▼
                           ┌──────────────┐
                           │  Order Book  │
                           │ JSON Output  │
                           └──────────────┘
```

**Setup:** See [docs/SETUP_SRC.md](docs/SETUP_SRC.md)

---

### 2. **Server Version** (`/server/`)
Web-based interface with three microservices running on a single machine.

```
┌──────────────┐
│   Browser    │
│ (Port 8000)  │
└──────┬───────┘
       │ HTTP
       ▼
┌──────────────────┐
│  Python FastAPI  │
│     Server       │
└─────┬──────┬─────┘
      │ HTTP │ HTTP
      ▼      ▼
┌─────────┐ ┌──────────┐
│ Sender  │ │ Receiver │
│  :8081  │ │  :8082   │
└─────────┘ └──────────┘
```

**Setup:** See [docs/SETUP_SERVER.md](docs/SETUP_SERVER.md)

---

### 3. **Microservices Version** (`/microservices/`)
Production-ready, containerized architecture with Docker.

```
┌──────────────┐
│   Browser    │
└──────┬───────┘
       │ HTTP
       ▼
┌──────────────────┐
│ Python Container │
└─────┬──────┬─────┘
      │      │
      ▼      ▼
┌─────────┐ ┌──────────┐
│ Sender  │ │ Receiver │
│Container│ │Container │
└────┬────┘ └────┬─────┘
     │           │
     └─────┬─────┘
           ▼
    ┌──────────────┐
    │Shared Volume │
    └──────────────┘
```

**Setup:** See [docs/SETUP_MICROSERVICES.md](docs/SETUP_MICROSERVICES.md)

---

## 📊 Output Format & Metrics

All three versions produce identical output and metrics.

### JSON Output Format

**File Location:**
- Standalone: `/src/data/order_book_output.json`
- Server: `/server/data/order_book_output.json`
- Microservices: `/microservices/shared-data/order_book_output.json`

**Format:** NDJSON (Newline Delimited JSON)

```json
{
  "symbol": "CLX5",
  "timestamp": "2025-09-24T19:30:00.000860000Z",
  "timestamp_ns": 1758742200000860000,
  "bbo": {
    "bid": {
      "price": 64780000000,
      "size": 3,
      "count": 1
    },
    "ask": {
      "price": 65400000000,
      "size": 1,
      "count": 1
    }
  },
  "stats": {
    "total_orders": 147,
    "bid_levels": 61,
    "ask_levels": 52,
    "best_bid": "64.78 @ 3 (1 orders)",
    "best_ask": "65.40 @ 1 (1 orders)",
    "bid_ask_spread": 620000000
  }
}
```

---

## 🛠️ Technology Stack

### Core Technologies
- **C++17** - High-performance data processing
- **Python 3.8+** - Web server and orchestration (server/microservices versions)
- **FastAPI** - Modern web framework (server/microservices versions)
- **Docker** - Containerization (microservices version)

### Key Libraries
- **databento-cpp** - Market data types and utilities
- **CMake** - Build system
- **uvicorn** - ASGI server (Python versions)

---

## 📁 Data File Setup

**IMPORTANT:** Before running any version, ensure the DBN market data file is in the correct location:

### Data File Locations

| Version | Data File Path | Description |
|---------|---------------|-------------|
| **Standalone** (`/src/`) | `/src/data/CLX5_mbo.dbn` | Local data directory |
| **Server** (`/server/`) | `/server/data/CLX5_mbo.dbn` | Server data directory |
| **Microservices** (`/microservices/`) | `/microservices/shared-data/CLX5_mbo.dbn` | Shared Docker volume |

---

## 📦 Installing databento-cpp (Required for `/src/` and `/server/`)

The **standalone** and **server** versions require the databento-cpp library to be installed in the **project root**.

> **Note:** The microservices version does NOT require this - it's handled by Docker.

### Installation Steps

```bash
# Navigate to project root
cd /Users/hang/github_repo/assignment

# Clone databento-cpp
git clone https://github.com/databento/databento-cpp.git

# Build and install
cd databento-cpp
mkdir build && cd build
cmake ..
make -j$(nproc)  # Linux
# OR
make -j$(sysctl -n hw.ncpu)  # macOS

# Install (may require sudo)
sudo make install

# Verify installation
ls /usr/local/include/databento/
# Should see: record.hpp, enums.hpp, constants.hpp, etc.
```

## 📚 Detailed Setup Instructions

- **Standalone Version:** [docs/SETUP_SRC.md](docs/SETUP_SRC.md)
- **Server Version:** [docs/SETUP_SERVER.md](docs/SETUP_SERVER.md)
- **Microservices Version:** [docs/SETUP_MICROSERVICES.md](docs/SETUP_MICROSERVICES.md)

---

## 🎯 Feature Comparison

| Feature | Standalone | Server | Microservices |
|---------|-----------|--------|---------------|
| **Web Interface** | ❌ | ✅ | ✅ |
| **Real-time Metrics** | Console only | Web UI | Web UI |
| **Setup Complexity** | Simple | Medium | Simple (Docker) |
| **Dependencies** | C++ only | C++ + Python | Docker |
| **Isolation** | Single process | Multiple processes | Containers |
| **Scalability** | Single machine | Single machine | Horizontal scaling |
| **Production Ready** | ❌ | ⚠️ | ✅ |
| **Cloud Deployment** | ❌ | ⚠️ | ✅ |

---

## 🔧 System Requirements

### All Versions
- **OS:** Linux or macOS
- **Compiler:** GCC 11+ or Clang 14+ with C++17 support
- **CMake:** Version 3.15+
- **Memory:** At least 2GB RAM
- **Disk:** 500MB free space

### Server Version (Additional)
- **Python:** 3.8+
- **pip:** Latest version

### Microservices Version (Additional)
- **Docker:** 20.10+
- **Docker Compose:** 2.0+
- **Memory:** 4GB RAM allocated to Docker

---




## 📖 Architecture Overview

### Data Flow

```
DBN File → TCP Sender → TCP Receiver → Order Book → JSON Output
   ↓           ↓            ↓             ↓            ↓
Market      Streams      Receives    Maintains    Snapshots
 Data       Messages     Messages    Live Book    to File
```

### Order Book Processing

1. **Message Reception:** TCP receiver gets MBO messages
2. **Order Book Update:** Each message updates the order book state
3. **JSON Generation:** Order book snapshot written after each update
4. **Batched Flushing:** JSON records flushed in batches for performance

### Key Features

- ✅ **High Performance:** Processes ~50,000 messages/sec
- ✅ **Real-time Updates:** Order book updated with every message
- ✅ **Complete History:** Every order book state captured
- ✅ **Efficient Storage:** Batched JSON writes with configurable flushing
- ✅ **Thread Safe:** Mutex-protected buffer operations
- ✅ **Graceful Shutdown:** Ensures all data is flushed on exit
