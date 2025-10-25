#!/bin/bash

# Navigate to the server directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
cd "$SCRIPT_DIR"

echo "🌐 Starting Order Book Microservices..."

# Kill any existing processes first
echo "🔄 Stopping existing processes..."
pkill -f sender_microservice 2>/dev/null || true
pkill -f receiver_microservice 2>/dev/null || true
pkill -f "python main.py" 2>/dev/null || true
sleep 2

# Check if Python is installed
if ! command -v python3 &> /dev/null; then
    echo "❌ Python 3 is not installed. Please install Python 3 first."
    exit 1
fi

# Check if pip is installed
if ! command -v pip3 &> /dev/null; then
    echo "❌ pip3 is not installed. Please install pip3 first."
    exit 1
fi

# Install requirements if needed
if [ ! -d "venv" ]; then
    echo "📦 Creating virtual environment..."
    python3 -m venv venv
fi

echo "🔧 Activating virtual environment..."
source venv/bin/activate

echo "📥 Installing Python dependencies..."
if ! pip show fastapi > /dev/null 2>&1; then
    pip install -r requirements.txt
else
    echo "✅ Python dependencies already installed"
fi

# Build C++ microservices
echo "🛠️ Building C++ microservices..."
mkdir -p build
cd build
cmake ..
make
cd ..

# Start C++ Sender Microservice in background (from server directory)
echo "🚀 Starting C++ Sender Microservice (port 8081)..."
(cd . && ./build/sender_microservice) &
SENDER_PID=$!
echo "Sender Microservice started with PID: $SENDER_PID"

# Start C++ Receiver Microservice in background (from server directory)
echo "📥 Starting C++ Receiver Microservice (port 8082)..."
(cd . && ./build/receiver_microservice) &
RECEIVER_PID=$!
echo "Receiver Microservice started with PID: $RECEIVER_PID"

# Wait a moment for services to start
sleep 2

# Start the FastAPI application
echo "🚀 Starting FastAPI application (port 8000)..."
echo "🔗 Access the web interface at: http://localhost:8000"
echo "📚 API documentation at: http://localhost:8000/docs"
echo ""
echo "Microservices:"
echo "  📡 Sender Service: http://localhost:8081"
echo "  📥 Receiver Service: http://localhost:8082"
echo "  🌐 Python Server: http://localhost:8000"
echo ""

python main.py

# Ensure background processes are killed on exit
trap "kill $SENDER_PID $RECEIVER_PID" EXIT