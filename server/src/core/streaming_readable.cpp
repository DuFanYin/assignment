#include "project/streaming_readable.hpp"

#include <algorithm>
#include <cstring>

#include "databento/exceptions.hpp"

void StreamingBufferState::append(const uint8_t* data, std::size_t length) {
    append(reinterpret_cast<const std::byte*>(data), length);
}

void StreamingBufferState::append(const std::byte* data, std::size_t length) {
    if (length == 0) {
        return;
    }
    std::vector<std::byte> chunk(length);
    std::memcpy(chunk.data(), data, length);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_) {
            return;
        }
        chunks_.emplace_back(std::move(chunk));
        totalBytes_ += length;
    }
    cv_.notify_all();
}

void StreamingBufferState::finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
}

bool StreamingBufferState::isFinished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_ && chunks_.empty();
}

std::size_t StreamingBufferState::totalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

StreamingReadable::StreamingReadable(std::shared_ptr<StreamingBufferState> state)
    : state_(std::move(state)) {
    if (!state_) {
        throw std::invalid_argument("StreamingReadable requires a non-null state");
    }
}

void StreamingReadable::ReadExact(std::byte* buffer, std::size_t length) {
    std::size_t copied = 0;
    while (copied < length) {
        const auto read = ReadSome(buffer + copied, length - copied);
        if (read == 0) {
            throw databento::DbnResponseError{"Unexpected end of streaming input"};
        }
        copied += read;
    }
}

std::size_t StreamingReadable::ReadSome(std::byte* buffer, std::size_t max_length) {
    if (max_length == 0) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(state_->mutex_);
    state_->cv_.wait(lock, [this]() {
        return !state_->chunks_.empty() || state_->finished_;
    });

    if (state_->chunks_.empty()) {
        return 0;
    }

    auto& chunk = state_->chunks_.front();
    const auto available = chunk.size() - offset_;
    const auto toCopy = std::min<std::size_t>(max_length, available);
    std::memcpy(buffer, chunk.data() + offset_, toCopy);
    offset_ += toCopy;

    if (offset_ == chunk.size()) {
        state_->chunks_.pop_front();
        offset_ = 0;
    }

    return toCopy;
}

