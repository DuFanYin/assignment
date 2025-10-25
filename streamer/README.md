# TCP Market Data Streaming System

This directory contains a TCP-based market data streaming system that follows the same data reader approach as the main processor, but streams data over TCP instead of reading directly from files.

## Architecture

```
DBN File → TCP Sender → Network → TCP Receiver → Order Book
```

## Components

### 1. TCP Sender (`tcp_sender.hpp`, `tcp_sender.cpp`)
- **Purpose**: Reads DBN files and streams MBO data over TCP
- **Features**:
  - Multi-client support
  - Configurable delay between orders
  - Continuous streaming (restarts file when EOF reached)
  - Real-time statistics

### 2. TCP Receiver (`tcp_receiver.hpp`, `tcp_receiver.cpp`)
- **Purpose**: Receives MBO data over TCP and processes it through order book
- **Features**:
  - Same order book processing as main processor
  - Graceful error handling for missing orders
  - Real-time order book statistics
  - Performance metrics

### 3. Example Programs
- **`sender_main.cpp`**: TCP sender example
- **`receiver_main.cpp`**: TCP receiver example with order book

## Usage

### Build the System
```bash
cd streamer
mkdir build
cd build
cmake ..
make
```

### Run TCP Sender
```bash
./tcp_sender
```
- Reads from `../src/data/CLX5_mbo.dbn`
- Listens on `127.0.0.1:8080`
- Streams orders with 1ms delay

### Run TCP Receiver
```bash
./tcp_receiver
```
- Connects to `127.0.0.1:8080`
- Processes orders through order book
- Displays real-time statistics

## Data Flow

### Sender Side
1. Load DBN file using Databento library
2. Read MBO records sequentially
3. Serialize MBO data to text format
4. Send over TCP to connected clients
5. Restart file when EOF reached

### Receiver Side
1. Connect to TCP sender
2. Receive MBO messages
3. Parse messages back to MboMsg format
4. Process through order book (same as main processor)
5. Display statistics and order book state

## Message Format

TCP messages use a simple text format:
```
MBO:order_id:price:size:action:side
```

Example:
```
MBO:8058566314544:64830000000:1:A:A
```

## Key Features

### Same Data Reader Approach
- Uses identical Databento library for file reading
- Same MBO record processing
- Same error handling for missing orders
- Same order book implementation

### TCP Streaming Benefits
- **Real-time**: Simulates live market data feeds
- **Network**: Can stream to remote clients
- **Scalable**: Multiple receivers can connect
- **Configurable**: Adjustable delay and port settings

### Performance Characteristics
- **Single-threaded**: Both sender and receiver
- **Low latency**: Direct TCP communication
- **Efficient**: Minimal serialization overhead
- **Reliable**: Graceful error handling

## Configuration

### Sender Configuration
```cpp
sender->setHost("127.0.0.1");     // Server IP
sender->setPort(8080);            // Server port
sender->setDelayMs(1);            // Delay between orders (ms)
```

### Receiver Configuration
```cpp
receiver->setHost("127.0.0.1");   // Server IP
receiver->setPort(8080);          // Server port
```

## Comparison with File-Based Processing

| Aspect | File-Based | TCP Streaming |
|--------|------------|---------------|
| **Data Source** | Direct file access | Network stream |
| **Latency** | File I/O bound | Network bound |
| **Scalability** | Single process | Multiple clients |
| **Real-time** | Batch processing | Live streaming |
| **Use Case** | Backtesting | Live trading |

## Future Enhancements

1. **Binary Protocol**: Replace text format with binary for better performance
2. **Compression**: Add gzip compression for network efficiency
3. **Authentication**: Add client authentication
4. **Multiple Instruments**: Support multiple symbols
5. **Heartbeat**: Add connection health monitoring
6. **Reconnection**: Automatic reconnection on connection loss

## Testing

### Basic Test
1. Start sender: `./tcp_sender`
2. Start receiver: `./tcp_receiver`
3. Verify orders are being processed
4. Check order book statistics

### Performance Test
- Monitor orders/second processing rate
- Check memory usage
- Verify order book accuracy

### Stress Test
- Multiple receivers connecting to one sender
- Long-running sessions
- Network interruption recovery
