#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "databento/ireadable.hpp"

/**
 * Thread-safe buffer for streaming binary data chunks.
 * Chunks are appended by the WebSocket thread and consumed by the processing thread.
 */
class StreamBuffer {
public:
    StreamBuffer() = default;

    // Append a chunk of data to the buffer
    void appendChunk(const uint8_t* data, std::size_t length);
    void appendChunk(const std::byte* data, std::size_t length);
    
    // Signal that no more data will be appended
    void markFinished();
    
    // Check if all data has been consumed
    [[nodiscard]] bool isFullyConsumed() const;
    
    // Get total bytes appended (for statistics)
    [[nodiscard]] std::size_t getTotalBytes() const;

private:
    friend class StreamReader;

    mutable std::mutex bufferMutex_;
    std::condition_variable dataAvailableCondition_;
    std::deque<std::vector<std::byte>> dataChunks_;
    bool isFinished_{false};
    std::size_t totalBytesAppended_{0};
};

/**
 * Readable interface for consuming data from StreamBuffer.
 * Implements databento::IReadable for use with DbnDecoder.
 */
class StreamReader : public databento::IReadable {
public:
    explicit StreamReader(std::shared_ptr<StreamBuffer> buffer);

    void ReadExact(std::byte* buffer, std::size_t length) override;
    std::size_t ReadSome(std::byte* buffer, std::size_t max_length) override;

private:
    std::shared_ptr<StreamBuffer> buffer_;
    std::size_t currentChunkOffset_{0};  // Offset within current chunk being read
};

