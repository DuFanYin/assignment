# Setup Guide - Server Version (`/server/`)

> **💡 TIP:** Use the provided `start.sh` script for the easiest setup! See [Quick Start](#quick-start-4-steps) below.

> **📖 For architecture overview and output format:** See main [README.md](../README.md)

## Prerequisites

### System Requirements
- **OS**: Linux or macOS
- **Compiler**: GCC 11+ or Clang 14+ with C++17 support
- **CMake**: Version 3.15 or higher
- **Python**: Version 3.8 or higher
- **Memory**: At least 4GB RAM
- **Disk**: 1GB free space

### Required Tools
```bash
# macOS
brew install cmake python3

# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential cmake git python3 python3-pip python3-venv
```

## Quick Start (4 Steps)

### Step 1: Install databento-cpp Library

See [main README.md - Installing databento-cpp](../README.md#-installing-databento-cpp-required-for-src-and-server) for installation instructions.

### Step 2: Set Up Python Environment

```bash
cd /Users/hang/github_repo/assignment/server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

### Step 3: Ensure Data File Exists

The DBN market data file should be at:
```
📁 /Users/hang/github_repo/assignment/server/data/CLX5_mbo.dbn
```

> **📁 For detailed data file setup instructions:** See main [README.md - Data File Setup](../README.md#-data-file-setup)

### Step 4: Run the Start Script

```bash
cd /Users/hang/github_repo/assignment/server
./start.sh
```

**That's it!** The script will:
1. Build C++ microservices
2. Start sender (Port 8081)
3. Start receiver (Port 8082)
4. Start Python server (Port 8000)
5. Open http://localhost:8000 in your browser

Click **"Start Streaming"** button to process market data.

---

## What the Start Script Does

```bash
✅ Kills processes on ports 8000, 8081, 8082
✅ Builds C++ sender and receiver microservices
✅ Starts all three services
✅ Opens web browser automatically
✅ Shows service logs
```

## Using the Web Interface

### Frontend Features

1. **Start Streaming Button**
   - Click to begin processing market data
   - Button shows loading state during processing

2. **Processing Statistics**
   - Processing Time (ms)
   - Messages Received
   - Orders Processed
   - JSON Records Generated
   - Throughput (msg/sec)
   - File Size (MB)

3. **Order Book Summary**
   - Total Records
   - Active Orders
   - Bid/Ask Price Levels
   - Best Bid/Ask with sizes
   - Bid-Ask Spread
   - Order Processing Rate

4. **Order Book Data Display**
   - JSON view of the final order book state
   - Scrollable view for large datasets

### API Endpoints

The server exposes the following REST API endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Serves the web frontend |
| `/api/status` | GET | Check system status |
| `/api/start-streaming` | POST | Start data processing |
| `/api/order-book` | GET | Get order book data |

**Example API Usage:**
```bash
# Check status
curl http://localhost:8000/api/status

# Start streaming
curl -X POST http://localhost:8000/api/start-streaming

# Get order book
curl http://localhost:8000/api/order-book
```

## Output Files

**Location:** `/Users/hang/github_repo/assignment/server/data/order_book_output.json`

**Format & Metrics:** See main [README.md](../README.md#-output-format--metrics)

## Troubleshooting

### Build Errors

**Error: `databento/record.hpp: No such file or directory`**
```bash
# Solution: Install databento-cpp in project root
cd /Users/hang/github_repo/assignment
git clone https://github.com/databento/databento-cpp.git
cd databento-cpp/build
cmake .. && make && sudo make install
```

**Error: `CMake Error: Could not find CMAKE_CXX_COMPILER`**
```bash
# macOS: Install Xcode Command Line Tools
xcode-select --install

# Linux: Install build-essential
sudo apt-get install build-essential
```

### Python Errors

**Error: `ModuleNotFoundError: No module named 'fastapi'`**
```bash
# Activate virtual environment and install dependencies
cd /Users/hang/github_repo/assignment/server
source venv/bin/activate
pip install -r requirements.txt
```

**Error: `Address already in use` (Port 8000, 8081, or 8082)**
```bash
# Kill processes on those ports
lsof -ti:8000 | xargs kill -9
lsof -ti:8081 | xargs kill -9
lsof -ti:8082 | xargs kill -9

# Or use the start script which does this automatically
./start.sh
```

### Runtime Errors

**Error: Frontend shows zeros for all metrics**
- Ensure you're using the latest code with the throughput calculation fix
- Check browser console for JavaScript errors (F12)
- Verify C++ microservices are running: `ps aux | grep microservice`

**Error: `Failed to connect to sender/receiver microservice`**
```bash
# Check if microservices are running
lsof -i :8081  # Sender
lsof -i :8082  # Receiver

# Restart microservices
./start.sh
```

**Error: `No such file or directory: 'static/index.html'`**
```bash
# Ensure static directory and index.html exist
ls /Users/hang/github_repo/assignment/server/static/index.html

# If missing, check if it's in the repository
git status
```

### Frontend Issues

**Issue: Metrics not updating**
- Hard refresh the browser (Ctrl+Shift+R or Cmd+Shift+R)
- Check browser console for errors
- Verify API response: `curl http://localhost:8000/api/status`

**Issue: "Start Streaming" button doesn't work**
- Check Python server logs for errors
- Verify all three services are running
- Check network tab in browser dev tools (F12)

## Performance Tuning

### Optimize C++ Build
```bash
cd /Users/hang/github_repo/assignment/server/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make clean && make -j$(nproc)
```

### Adjust JSON Batching
Edit `server/receiver_microservice.cpp`:
```cpp
receiver_->setJsonBatchSize(10000);      // Larger batches (default: 5000)
receiver_->setJsonFlushInterval(1000);   // Less frequent flushes (default: 500)
```

### Python Server Performance
```bash
# Use more worker processes
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4
```

## Stopping the System

### Using Ctrl+C
Press `Ctrl+C` in the terminal running `start.sh` to gracefully stop all services.

### Manual Cleanup
```bash
# Kill all related processes
pkill -f sender_microservice
pkill -f receiver_microservice
pkill -f "uvicorn main:app"

# Or kill by port
lsof -ti:8000,8081,8082 | xargs kill -9
```

## Next Steps

- **Try standalone version**: See [SETUP_SRC.md](./SETUP_SRC.md)
- **Try microservices with Docker**: See [SETUP_MICROSERVICES.md](./SETUP_MICROSERVICES.md)
- **View architecture & metrics**: See main [README.md](../README.md)

## Support

For issues or questions:
1. Check the troubleshooting section above
2. Review the main README.md
3. Examine server logs in the terminal
4. Check browser console (F12) for frontend errors

