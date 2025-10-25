#!/bin/bash

# Order Book Streamer - Standalone Version Startup Script

echo "🚀 Starting Order Book Streamer (Standalone Version)..."

# Navigate to src directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")")
cd "$SCRIPT_DIR"


# Check if data file exists
if [ ! -f "data/CLX5_mbo.dbn" ]; then
    echo "❌ Data file not found: data/CLX5_mbo.dbn"
    echo "Please ensure the DBN file is in the data/ directory"
    exit 1
fi

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Build the project
echo "🔨 Building sender and receiver..."
cmake .. > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "❌ CMake configuration failed"
    cmake ..
    exit 1
fi

make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2) > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "❌ Build failed"
    make
    exit 1
fi

echo "✅ Build successful"
echo ""

# Check if receiver and sender executables exist
if [ ! -f "./TCP_Receiver" ] || [ ! -f "./TCP_Sender" ]; then
    echo "❌ Executables not found after build"
    exit 1
fi

# Kill any existing receiver on port 8080
echo "🔄 Checking for existing processes on port 8080..."
lsof -ti:8080 | xargs kill -9 2>/dev/null
sleep 1

# Start sender in background first (it sets up the server)
echo "📤 Starting sender (TCP server)..."
./TCP_Sender > sender.log 2>&1 &
SENDER_PID=$!

# Wait for sender to set up the server
sleep 3

# Check if sender is still running
if ! kill -0 $SENDER_PID 2>/dev/null; then
    echo "❌ Sender failed to start. Check sender.log for details"
    cat sender.log
    exit 1
fi

echo "✅ Sender started (PID: $SENDER_PID)"
echo ""

# Start receiver (it connects to the sender)
echo "📥 Starting receiver (TCP client)..."
echo "======================================"
./TCP_Receiver

# Wait for receiver to complete
RECEIVER_EXIT=$?

echo "======================================"
echo ""

# Show sender output
echo "📤 Sender output:"
echo "======================================"
cat sender.log
echo "======================================"
echo ""

# Kill sender
echo "🛑 Stopping sender..."
kill $SENDER_PID 2>/dev/null
wait $SENDER_PID 2>/dev/null

# Check if output file was created
if [ -f "../data/order_book_output.json" ]; then
    FILE_SIZE=$(du -h ../data/order_book_output.json | cut -f1)
    RECORD_COUNT=$(wc -l < ../data/order_book_output.json)
    echo "✅ JSON output file created:"
    echo "   📁 Location: data/order_book_output.json"
    echo "   📊 Size: $FILE_SIZE"
    echo "   📝 Records: $RECORD_COUNT"
else
    echo "⚠️  JSON output file not found"
fi

echo ""
echo "✅ Processing completed!"

exit $RECEIVER_EXIT

