# Order Book Streamer

A high-performance market data processing system that streams Market-By-Order (MBO) data via TCP and builds a real-time order book with JSON output.

## 🚀 Three Implementation Versions

This project provides three different implementations to suit various use cases:

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

**Best for:**
- Local development and testing
- Learning the core functionality
- Debugging and performance analysis

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

**Best for:**
- Interactive web-based monitoring
- Single-machine deployment with web UI
- Development and demonstration

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

**Best for:**
- Production deployment
- Cloud environments (AWS, GCP, Azure)
- Scalable, isolated service architecture
- CI/CD pipelines

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

### Performance Metrics

All versions report the same metrics:

| Metric | Description | Typical Value |
|--------|-------------|---------------|
| **Processing Time** | Total time from start to finish | ~700-800 ms |
| **Messages Received** | Total MBO messages received | ~36,988 |
| **Orders Processed** | Orders successfully applied to book | ~36,988 |
| **JSON Records** | Order book snapshots written | ~36,988 |
| **Throughput** | Messages processed per second | ~51,000 msg/sec |
| **Order Processing Rate** | Orders processed per second | ~51,000 orders/sec |
| **File Size** | Size of JSON output file | ~42 MB |
| **Active Orders** | Orders currently in the order book | ~147 |
| **Bid Price Levels** | Number of bid price levels | ~61 |
| **Ask Price Levels** | Number of ask price levels | ~52 |
| **Bid-Ask Spread** | Difference between best bid and ask | Variable |

### Example Console Output

```
=== TCP Sender Final Statistics ===
Streaming Time: 507 ms
Messages Sent: 38212
Throughput: 75369 messages/sec
===================================

=== TCP Receiver Final Statistics ===
Processing Time: 721 ms
Messages Received: 36988
Orders Processed: 36988
JSON Records Generated: 36988
Message Throughput: 51301 messages/sec
Order Processing Rate: 51301 orders/sec

Final Order Book Summary:
  Active Orders: 147
  Bid Price Levels: 61
  Ask Price Levels: 52
  Best Bid: 64.78 @ 3 (1 orders)
  Best Ask: 65.40 @ 1 (1 orders)
  Bid-Ask Spread: 620000000
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

## 📋 Quick Start Guide

Choose your preferred version:

### Standalone Version
```bash
cd src
./start.sh
```

### Server Version
```bash
cd server
./start.sh
# Browser opens to http://localhost:8000
```

### Microservices Version
```bash
cd microservices
./start-docker.sh
# Browser opens to http://localhost:8000
```

---

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
| **Best Use Case** | Development | Demo/Testing | Production |

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