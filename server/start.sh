#!/bin/bash

# Navigate to the server directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
cd "$SCRIPT_DIR"

echo "🌐 Starting Order Book Microservices..."

# Kill any existing processes
lsof -ti:8000 | xargs kill -9 2>/dev/null || true
lsof -ti:8081 | xargs kill -9 2>/dev/null || true
lsof -ti:8082 | xargs kill -9 2>/dev/null || true
pkill -f sender_microservice 2>/dev/null || true
pkill -f receiver_microservice 2>/dev/null || true
pkill -f "python main.py" 2>/dev/null || true

echo "📋 Checking dependencies..."
if ! command -v python3 &> /dev/null; then
    echo "❌ Python 3 is not installed."
    exit 1
fi

# Initialize virtual environment
if [ ! -d "venv" ]; then
    echo "📦 Creating virtual environment..."
    python3 -m venv venv
fi

source venv/bin/activate

# Install requirements if needed
if ! pip show fastapi > /dev/null 2>&1; then
    echo "📥 Installing dependencies..."
    pip install -r requirements.txt
fi

# Build C++ microservices
echo "🔨 Building C++ microservices..."
mkdir -p build
cd build
if [ ! -f "CMakeCache.txt" ]; then
    cmake ..
fi
make -j4
cd ..

# Start microservices in background (logs visible on terminal)
echo "🚀 Starting sender microservice (port 8081)..."
./build/sender_microservice &
SENDER_PID=$!

echo "📥 Starting receiver microservice (port 8082)..."
./build/receiver_microservice &
RECEIVER_PID=$!

echo "⏳ Waiting for microservices to start..."
sleep 2

# Check if microservices started successfully
if ! kill -0 $SENDER_PID 2>/dev/null; then
    echo "❌ Sender failed to start"
    exit 1
fi

if ! kill -0 $RECEIVER_PID 2>/dev/null; then
    echo "❌ Receiver failed to start"
    exit 1
fi

# Start FastAPI
echo "🌐 Starting FastAPI server on http://localhost:8000"
echo ""

python main.py

# Ensure background processes are killed on exit
trap "kill $SENDER_PID $RECEIVER_PID" EXIT