#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "databento/ireadable.hpp"

class StreamingBufferState {
public:
    StreamingBufferState() = default;

    void append(const uint8_t* data, std::size_t length);
    void append(const std::byte* data, std::size_t length);
    void finish();

    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] std::size_t totalBytes() const;

private:
    friend class StreamingReadable;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<std::byte>> chunks_;
    bool finished_{false};
    std::size_t totalBytes_{0};
};

class StreamingReadable : public databento::IReadable {
public:
    explicit StreamingReadable(std::shared_ptr<StreamingBufferState> state);

    void ReadExact(std::byte* buffer, std::size_t length) override;
    std::size_t ReadSome(std::byte* buffer, std::size_t max_length) override;

private:
    std::shared_ptr<StreamingBufferState> state_;
    std::size_t offset_{0};
};

