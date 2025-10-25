# Market Data Processor Architecture

## Overview
This is a single-threaded market data processor that reads DBN (DataBento) files and maintains a real-time limit order book. It processes Market-by-Order (MBO) data for commodity futures trading.

## Architecture Components

### 1. Data Flow
```
DBN File → Streamer → Order Book → Statistics Output
```

### 2. Core Components

#### **Streamer (`src/include/streamer.hpp`, `src/src/streamer.cpp`)**
- **Purpose**: Reads DBN files and provides sequential access to MBO records
- **Key Methods**:
  - `loadFromFile()`: Opens and validates DBN file
  - `getNextRecord()`: Returns next record from file (single-threaded access)
- **Data Format**: Uses official Databento C++ library for DBN parsing

#### **Order Book (`src/include/order_book.hpp`)**
- **Purpose**: Maintains limit order book state using official Databento implementation
- **Data Structures**:
  - AVL trees for bid/ask price levels (O(log n) operations)
  - Hash maps for order lookups (O(1) operations)
  - Price-time priority ordering
- **Key Methods**:
  - `Apply(mbo)`: Processes individual orders (ADD/CANCEL/MODIFY)
  - `Bbo()`: Returns best bid/ask
  - `GetOrderCount()`, `GetBidLevelCount()`, `GetAskLevelCount()`: Statistics

#### **Main Processor (`src/src/main.cpp`)**
- **Purpose**: Orchestrates the entire processing pipeline
- **Processing Loop**:
  ```cpp
  while (true) {
      record = streamer->getNextRecord();
      if (!record) break;
      
      orderBook->Apply(mbo);  // Process ONE order completely
      orderCount++;
  }
  ```

## Processing Model

### Single-Threaded Sequential Processing
- **One Order at a Time**: Each order is completely processed before the next
- **Atomic Operations**: Each order operation is atomic (complete or fail)
- **State Consistency**: Order book maintains consistent state after each operation

### Order Processing Flow
```
Order 1: ADD order_id=123, side=B, price=100, size=50
├── Validate order
├── Add to bid tree
├── Update order map
├── Update best bid/ask
└── ✅ COMPLETE

Order 2: CANCEL order_id=123, side=B, price=100, size=25
├── Find order in map
├── Reduce size
├── Update tree structure
├── Update best bid/ask
└── ✅ COMPLETE
```

## Data Handling

### Market Data Characteristics
- **Symbol**: CLX5 (Crude Oil futures)
- **Schema**: MBO (Market-by-Order)
- **Actions**: ADD ('A'), CANCEL ('C'), MODIFY ('M'), FILL ('F'), TRADE ('T')
- **Sides**: BUY ('B'), SELL ('A')

### Error Handling
- **Missing Orders**: Gracefully skip orders referencing non-existent orders
- **Unknown Levels**: Skip orders at non-existent price levels
- **Data Gaps**: Common in real market data - handled by skipping invalid operations

## Performance Characteristics

### Timing Metrics
- **Processing Time**: Measured in milliseconds
- **Orders per Second**: Calculated as `orders * 1000 / time_ms`
- **Single-threaded**: No mutex/locking overhead
- **Sequential**: Each order waits for previous to complete

### Expected Performance
- **Target**: ~1M orders/second (similar to Limit-Order-Book reference)
- **Latency**: Microsecond-level per order
- **Memory**: Efficient tree structures with minimal overhead

## Key Design Decisions

### 1. Single-Threaded Architecture
**Why**: 
- Eliminates race conditions by design
- Simpler code and debugging
- Better performance (no synchronization overhead)
- Mirrors real trading system requirements

### 2. Official Databento Order Book
**Why**:
- Production-ready implementation
- Handles complex order book operations correctly
- Maintains price-time priority
- Robust error handling

### 3. Sequential Processing
**Why**:
- Ensures order book consistency
- Easy to reason about and debug
- Matches real market data processing requirements
- Atomic operations prevent partial state

### 4. Graceful Error Handling
**Why**:
- Real market data has gaps and missing orders
- System continues processing valid data
- Logs issues without crashing
- Mirrors production trading systems

## File Structure
```
src/
├── include/
│   ├── order_book.hpp    # Official Databento order book
│   ├── streamer.hpp      # DBN file reader
│   ├── order.hpp         # Order data structures
│   └── utils.hpp         # Utility functions
├── src/
│   ├── main.cpp          # Main processing loop
│   ├── streamer.cpp      # Streamer implementation
│   └── utils.cpp         # Utility implementations
└── CMakeLists.txt        # Build configuration
```

## Future Extensions

### TCP Streaming
The architecture is designed to easily extend to TCP streaming:
```cpp
// Future TCP streaming architecture
while (true) {
    MboMsg mbo = tcpSocket.readNextOrder();
    orderBook->Apply(mbo);
    
    if (mbo.flags.IsLast()) {
        broadcastOrderBookUpdate(orderBook->Bbo());
    }
}
```

### Multiple Instruments
The order book can handle multiple instruments by using the `Market` class with instrument-specific books.

### Real-time Updates
The single-threaded design allows for easy addition of real-time update broadcasting to connected clients.

## Comparison with Multi-threaded Approach

| Aspect | Single-Threaded (Current) | Multi-threaded (Previous) |
|--------|---------------------------|---------------------------|
| **Performance** | ~1M orders/sec | Limited by mutex overhead |
| **Complexity** | Simple, linear | Complex synchronization |
| **Race Conditions** | Impossible | Must be managed |
| **Debugging** | Easy to trace | Complex thread interactions |
| **Real-time** | Perfect for streaming | Overhead from synchronization |

## Conclusion

This single-threaded architecture provides:
- **High Performance**: No synchronization overhead
- **Correctness**: Impossible race conditions
- **Simplicity**: Easy to understand and maintain
- **Scalability**: Ready for TCP streaming and multiple instruments
- **Production Ready**: Based on official Databento implementation

The design prioritizes correctness and performance over complexity, making it ideal for high-frequency market data processing.
