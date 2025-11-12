#include "project/streamer.hpp"

#include <algorithm>
#include <cstring>

#include "databento/exceptions.hpp"

void StreamBuffer::appendChunk(const uint8_t* data, std::size_t length) {
    appendChunk(reinterpret_cast<const std::byte*>(data), length);
}

void StreamBuffer::appendChunk(const std::byte* data, std::size_t length) {
    if (length == 0) {
        return;
    }
    
    // Create a copy of the chunk data
    std::vector<std::byte> chunkData(length);
    std::memcpy(chunkData.data(), data, length);

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (isFinished_) {
            return;  // Don't accept new data after finished
        }
        dataChunks_.emplace_back(std::move(chunkData));
        totalBytesAppended_ += length;
    }
    dataAvailableCondition_.notify_all();
}

void StreamBuffer::markFinished() {
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        isFinished_ = true;
    }
    dataAvailableCondition_.notify_all();
}

bool StreamBuffer::isFullyConsumed() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return isFinished_ && dataChunks_.empty();
}

std::size_t StreamBuffer::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    return totalBytesAppended_;
}

StreamReader::StreamReader(std::shared_ptr<StreamBuffer> buffer)
    : buffer_(std::move(buffer)) {
    if (!buffer_) {
        throw std::invalid_argument("StreamReader requires a non-null buffer");
    }
}

void StreamReader::ReadExact(std::byte* outputBuffer, std::size_t length) {
    std::size_t bytesCopied = 0;
    while (bytesCopied < length) {
        const auto bytesRead = ReadSome(outputBuffer + bytesCopied, length - bytesCopied);
        if (bytesRead == 0) {
            throw databento::DbnResponseError{"Unexpected end of streaming input"};
        }
        bytesCopied += bytesRead;
    }
}

std::size_t StreamReader::ReadSome(std::byte* outputBuffer, std::size_t maxLength) {
    if (maxLength == 0) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(buffer_->bufferMutex_);
    
    // Wait until data is available or stream is finished
    buffer_->dataAvailableCondition_.wait(lock, [this]() {
        return !buffer_->dataChunks_.empty() || buffer_->isFinished_;
    });

    if (buffer_->dataChunks_.empty()) {
        return 0;  // No more data available
    }

    // Read from the front chunk
    auto& currentChunk = buffer_->dataChunks_.front();
    const auto availableInChunk = currentChunk.size() - currentChunkOffset_;
    const auto bytesToCopy = std::min<std::size_t>(maxLength, availableInChunk);
    
    std::memcpy(outputBuffer, currentChunk.data() + currentChunkOffset_, bytesToCopy);
    currentChunkOffset_ += bytesToCopy;

    // If we've consumed the entire chunk, remove it and reset offset
    if (currentChunkOffset_ == currentChunk.size()) {
        buffer_->dataChunks_.pop_front();
        currentChunkOffset_ = 0;
    }

    return bytesToCopy;
}

