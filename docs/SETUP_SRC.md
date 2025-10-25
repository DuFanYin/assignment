# Setup Guide - Standalone Version (`/src/`)

> **💡 TIP:** Use the provided `start.sh` script for the easiest setup! See [Quick Start](#quick-start-3-steps) below.

> **📖 For architecture overview and output format:** See main [README.md](../README.md)

## Prerequisites

### System Requirements
- **OS**: Linux or macOS
- **Compiler**: GCC 11+ or Clang 14+ with C++17 support
- **CMake**: Version 3.15 or higher
- **Memory**: At least 2GB RAM
- **Disk**: 500MB free space

### Required Tools
```bash
# macOS
brew install cmake

# Ubuntu/Debian
sudo apt-get update
sudo apt-get install build-essential cmake git
```

## Quick Start (3 Steps)

### Step 1: Install databento-cpp Library

See [main README.md - Installing databento-cpp](../README.md#-installing-databento-cpp-required-for-src-and-server) for installation instructions.

### Step 2: Ensure Data File Exists

The DBN market data file should be at:
```
📁 /Users/hang/github_repo/assignment/src/data/CLX5_mbo.dbn
```

> **📁 For detailed data file setup instructions:** See main [README.md - Data File Setup](../README.md#-data-file-setup)

### Step 3: Run the Start Script

```bash
cd /Users/hang/github_repo/assignment/src
./start.sh
```

**That's it!** The script will:
1. Build sender and receiver
2. Start receiver in background
3. Start sender
4. Display real-time statistics

---

## What the Start Script Does

```bash
✅ Builds C++ executables
✅ Starts receiver on port 8080
✅ Starts sender
✅ Shows processing statistics
✅ Generates JSON output file
```

## Output Files

**Location:** `/Users/hang/github_repo/assignment/src/data/order_book_output.json`

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

### Runtime Errors

**Error: `Connection refused` or `Failed to connect`**
- Ensure the receiver is started **before** the sender
- Check that port 8080 is not in use: `lsof -i :8080`

**Error: `Permission denied` when writing JSON file**
```bash
# Create data directory with proper permissions
mkdir -p /Users/hang/github_repo/assignment/src/data
chmod 755 /Users/hang/github_repo/assignment/src/data
```

**Error: Missing records in JSON output**
- This has been fixed in the latest version
- Ensure you're using the updated code with proper JSON flushing

## Performance Tuning

### Optimize for Speed
```bash
# Build with optimizations
cd /Users/hang/github_repo/assignment/src/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make clean && make -j$(nproc)
```

### Adjust JSON Batching
Edit `src/src/receiver_main.cpp`:
```cpp
receiver->setJsonBatchSize(10000);      // Larger batches (default: 5000)
receiver->setJsonFlushInterval(1000);   // Less frequent flushes (default: 500)
```

## Next Steps

- **Try server version with web UI**: See [SETUP_SERVER.md](./SETUP_SERVER.md)
- **Try microservices with Docker**: See [SETUP_MICROSERVICES.md](./SETUP_MICROSERVICES.md)
- **View architecture & metrics**: See main [README.md](../README.md)

## Support

For issues or questions:
1. Check the troubleshooting section above
2. Review the main README.md
3. Examine the code comments in `/src/src/`

