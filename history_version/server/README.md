# Order Book Microservices

A microservice architecture for high-performance market data processing with order book reconstruction.

## Architecture

```
┌─────────────────┐    HTTP API    ┌─────────────────┐
│   Web Frontend  │ ──────────────► │  Python Server  │
│   (HTML/JS)     │                 │  (Port 8000)    │
└─────────────────┘                 └─────────────────┘
                                              │
                                              ▼ HTTP API
                                     ┌─────────────────┐
                                     │  C++ Sender     │
                                     │  Microservice   │
                                     │  (Port 8081)    │
                                     └─────────────────┘
                                              │
                                              ▼ HTTP API
                                     ┌─────────────────┐
                                     │  C++ Receiver   │
                                     │  Microservice   │
                                     │  (Port 8082)    │
                                     └─────────────────┘
```

## Services

### 1. Python Server (Port 8000)
- **FastAPI** web server
- Orchestrates microservices
- Provides web interface
- Handles HTTP API calls

### 2. C++ Sender Microservice (Port 8081)
- **Standalone** TCP sender service
- Reads market data from DBN files
- Streams data via TCP to receiver
- HTTP API for triggering

### 3. C++ Receiver Microservice (Port 8082)
- **Standalone** TCP receiver service
- Contains order book processing
- Generates JSON output
- HTTP API for processing and data retrieval

## Quick Start

1. **Start all services:**
   ```bash
   ./start.sh
   ```

2. **Access web interface:**
   - Main UI: http://localhost:8000
   - API Docs: http://localhost:8000/docs

3. **Individual service endpoints:**
   - Sender: http://localhost:8081
   - Receiver: http://localhost:8082

## Manual Setup

### Build C++ Microservices
```bash
mkdir build
cd build
cmake ..
make
```

### Start Services Individually

**Python Server:**
```bash
pip install -r requirements.txt
python main.py
```

**Sender Microservice:**
```bash
./build/sender_microservice
```

**Receiver Microservice:**
```bash
./build/receiver_microservice
```

## API Endpoints

### Python Server (Port 8000)
- `GET /` - Web interface
- `POST /api/start-streaming` - Trigger full streaming process
- `GET /api/order-book` - Get order book data
- `GET /api/microservices/status` - Check all services status

### Sender Microservice (Port 8081)
- `POST /start-streaming` - Start TCP streaming
- `GET /status` - Service status

### Receiver Microservice (Port 8082)
- `POST /start-processing` - Start order book processing
- `GET /order-book` - Get processed order book data
- `GET /status` - Service status

## Features

- **Microservice Architecture**: Independent, scalable services
- **High Performance**: C++ for data processing, Python for orchestration
- **Real-time Processing**: TCP streaming with order book reconstruction
- **JSON Output**: Batched JSON generation for optimal performance
- **Web Interface**: Modern HTML/JavaScript frontend
- **Health Monitoring**: Service status checking and monitoring

## Data Flow

1. **User clicks "Start Streaming"** in web interface
2. **Python Server** calls Sender Microservice
3. **Sender Microservice** streams market data via TCP
4. **Python Server** calls Receiver Microservice
5. **Receiver Microservice** processes data and builds order book
6. **Python Server** retrieves order book data
7. **Web Interface** displays results

## Performance

- **Sender**: ~50,000+ messages/sec
- **Receiver**: ~15,000+ messages/sec (with order book processing)
- **JSON Batching**: 5000 records per batch, flush every 500
- **Memory Efficient**: Streaming processing, no full data loading

## Dependencies

### Python
- FastAPI
- Uvicorn
- Requests

### C++
- C++17
- Threads
- Original src/ project components

## File Structure

```
server/
├── main.py                    # FastAPI server
├── sender_microservice.cpp    # C++ sender service
├── receiver_microservice.cpp  # C++ receiver service
├── CMakeLists.txt            # Build configuration
├── start.sh                  # Start all services
├── requirements.txt          # Python dependencies
├── static/
│   └── index.html           # Web interface
└── build/                   # C++ build output
```