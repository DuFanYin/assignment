# Dockerized Order Book Microservices

This directory contains 3 completely separate, dockerized microservices for the Order Book streaming system.

## Architecture

```
┌─────────────────┐    HTTP API    ┌─────────────────┐
│   Web Frontend  │ ──────────────► │  Python Server  │
│   (HTML/JS)     │                 │  (Container)    │
└─────────────────┘                 └─────────────────┘
                                              │
                                              ▼ HTTP API
                                     ┌─────────────────┐
                                     │  C++ Sender     │
                                     │  Microservice   │
                                     │  (Container)    │
                                     └─────────────────┘
                                              │
                                              ▼ HTTP API
                                     ┌─────────────────┐
                                     │  C++ Receiver   │
                                     │  Microservice   │
                                     │  (Container)    │
                                     └─────────────────┘
```

## Directory Structure

```
microservices/
├── python-server/              # Python FastAPI Server
│   ├── Dockerfile
│   ├── main.py
│   ├── requirements.txt
│   └── static/
│       └── index.html
├── sender-microservice/         # C++ Sender Service
│   ├── Dockerfile
│   ├── sender_microservice.cpp
│   ├── CMakeLists.txt
│   ├── src/
│   └── include/
├── receiver-microservice/       # C++ Receiver Service
│   ├── Dockerfile
│   ├── receiver_microservice.cpp
│   ├── CMakeLists.txt
│   ├── src/
│   └── include/
├── shared-data/                 # Shared data volume
├── docker-compose.yml           # Orchestration
└── start-docker.sh             # Startup script
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
   ./start-docker.sh
   ```

2. **Access web interface:**
   - Main UI: http://localhost:8000
   - API Docs: http://localhost:8000/docs

3. **Individual service endpoints:**
   - Sender: http://localhost:8081
   - Receiver: http://localhost:8082

## Manual Docker Commands

### Build and Start All Services
```bash
docker-compose up --build -d
```

### View Logs
```bash
docker-compose logs -f
```

### Stop Services
```bash
docker-compose down
```

### Restart Services
```bash
docker-compose restart
```

### Check Status
```bash
docker-compose ps
```

## Individual Container Management

### Build Individual Service
```bash
# Python Server
docker build -t python-server ./python-server

# Sender Microservice
docker build -t sender-microservice ./sender-microservice

# Receiver Microservice
docker build -t receiver-microservice ./receiver-microservice
```

### Run Individual Container
```bash
# Python Server
docker run -p 8000:8000 python-server

# Sender Microservice
docker run -p 8081:8081 sender-microservice

# Receiver Microservice
docker run -p 8082:8082 receiver-microservice
```

## Features

- **Containerized Architecture**: Each microservice runs in its own container
- **Service Discovery**: Containers communicate via Docker network
- **Shared Data Volume**: All services share the same data directory
- **Health Checks**: Each service has built-in health monitoring
- **Auto-restart**: Services restart automatically on failure
- **Scalable**: Each service can be scaled independently

## Data Flow

1. **User clicks "Start Streaming"** in web interface
2. **Python Server** calls Sender Microservice
3. **Sender Microservice** streams market data via TCP
4. **Python Server** calls Receiver Microservice
5. **Receiver Microservice** processes data and builds order book
6. **Python Server** retrieves order book data
7. **Web Interface** displays results

## Networking

- **Custom Network**: `orderbook-network`
- **Service Names**: 
  - `python-server`
  - `sender-microservice`
  - `receiver-microservice`
- **Shared Volume**: `shared-data`

## Environment Variables

### Python Server
- `SENDER_SERVICE_URL`: URL to sender microservice
- `RECEIVER_SERVICE_URL`: URL to receiver microservice

### Sender Microservice
- `DATA_FILE_PATH`: Path to DBN data file

### Receiver Microservice
- `OUTPUT_FILE_PATH`: Path to JSON output file
