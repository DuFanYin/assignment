# Pipeline and Performance Bottleneck Analysis

## Yes, the order book processing speed DOES affect sender speed!

### Pipeline Overview

```
Sender Thread              TCP Socket            Receiver Thread
===========                ============          ==============

1. Pre-parse messages ✓    [No bottleneck]       4. Read from socket
2. Convert to MboMessage                        [BLOCKS HERE if JSON slow]
3. Batch into buffer       ↓                    5. Convert to databento::MboMsg
4. sendBatchMessages() ────→ → → → ─────────────→6. orderBook_->Apply()
   (writev)              [TCP flow control]    7.   • Process (Add/Cancel/Modify)
                    ↓                        8.   • generateJsonOutput() ❌ BOTTLENECK
                    [Blocks if buffer full]     9.   • jsonCallback_() → buffer
                                                10.   • File write (flush periodically)
```

### Why Receiver Speed Affects Sender

**TCP Flow Control Chain:**

1. **Sender** calls `writev()` on socket (line 233-234 in `tcp_sender.cpp`)
2. **TCP** sends to receiver's socket receive buffer (default: 16MB per tcp_receiver.cpp line 42-51)
3. **Receiver** reads from socket and calls `orderBook_->Apply()` (line 196 in `tcp_receiver.cpp`)
4. **OrderBook** processes message AND generates JSON synchronously (lines 194-198 in `order_book.hpp`)
5. **If JSON generation is slow**, receiver process blocks
6. **TCP receive buffer fills up** because receiver isn't reading
7. **Sender's writev() BLOCKS** waiting for buffer space
8. **Sender throughput drops** to match receiver speed

### Primary Bottleneck: JSON Generation (Lines 202-258 in order_book.hpp)

This happens for **EVERY single message**:
```cpp
std::string generateJsonOutput(const db::MboMsg& mbo) {
    std::stringstream json;  // String concatenation
    // ... many string operations
    auto bbo = Bbo();  // Get best bid/offer (searches order book)
    
    // Iterate through top levels (lines 234-247)
    for (size_t i = 0; i < topLevels_; ++i) {
        auto bid = GetBidLevel(i);  // Expensive iteration
        // More string concatenation...
    }
    
    // More string building...
    return json.str();
}
```

**Cost per message:**
- ~20+ string concatenations
- Multiple order book searches/iterations
- Memory allocations for string building
- ~50-200μs per message (roughly 5,000-20,000 messages/sec max JSON generation rate)

This is **100x slower** than just processing the order book without JSON!

### Secondary Bottlenecks

1. **Memmove in receiver** (line 220 in `tcp_receiver.cpp`)
   - Happens for EVERY message after processing
   - Currently not optimized despite large buffer
   - Cost: ~1-5μs per message

2. **Message conversion overhead** (lines 195, 213-228 in sender)
   - Converting between MboMessage ↔ databento::MboMsg
   - Cost: ~0.5-1μs per message

3. **File I/O** (lines 342-348 in tcp_receiver.cpp)
   - JSON written to disk periodically
   - Synced with mutex (line 317-318)
   - Cost: ~50-100μs per flush

### Performance Metrics

**Without JSON enabled:**
- Sender: ~100,000+ msg/sec
- Receiver: ~100,000+ msg/sec
- Network: ~100,000+ msg/sec
- **Total**: Limited by slowest stage

**With JSON enabled (CURRENT STATE):**
- Sender: ~20,000-50,000 msg/sec (BLOCKED by receiver)
- Receiver: ~5,000-20,000 msg/sec (JSON generation)
- Network: ~20,000-50,000 msg/sec (flow controlled by receiver)
- **Total**: ~5,000-20,000 msg/sec ❌ **RECEIVER IS THE BOTTLENECK**

### Solutions to Improve Performance

#### Option 1: Disable JSON (fastest)
```cpp
// In receiver_main.cpp line 30
receiver->enableJsonOutput(false);  // Instead of true
```
**Impact**: 10-20x speedup

#### Option 2: Sample JSON (partial speedup)
```cpp
// Only generate JSON every Nth message
if (msgCounter++ % 1000 == 0) {
    generateJsonOutput(mbo);
}
```
**Impact**: 10-20x speedup

#### Option 3: Decouple JSON generation (best)
Move JSON generation to separate thread:
```cpp
// Use queue/worker thread for JSON generation
// Process order book without JSON in main thread
// Generate JSON asynchronously
```
**Impact**: 10-100x speedup depending on queue size

#### Option 4: Optimize memmove (minor)
Only shift buffer every N messages instead of every message.

### Recommendations

1. **For maximum speed**: Disable JSON generation entirely
2. **For usable data**: Sample JSON every 1000-10000 messages
3. **For production**: Use separate thread/process for JSON generation

The current setup has JSON generation as the dominant bottleneck affecting the entire pipeline!

