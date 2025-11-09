#include "project/json_writer.hpp"

#include <cstring>
#include <stdexcept>
#include <fstream>

class MmapJsonWriter::Impl {
public:
    explicit Impl(const std::string& path)
        : stream(path, std::ios::out | std::ios::app | std::ios::binary) {}
    std::ofstream stream;
};

MmapJsonWriter::MmapJsonWriter(const std::string& filePath,
                               std::size_t /*initialCapacityBytes*/,
                               std::size_t flushBatchLines)
    : filePath_(filePath),
      flushBatchLines_(flushBatchLines),
      linesSinceFlush_(0),
      impl_(new Impl(filePath)) {
    if (!impl_->stream.is_open()) {
        delete impl_;
        impl_ = nullptr;
        throw std::runtime_error("Failed to open JSON output file");
    }
}

MmapJsonWriter::~MmapJsonWriter() {
    if (impl_) {
        impl_->stream.flush();
        delete impl_;
        impl_ = nullptr;
    }
}

void MmapJsonWriter::appendLine(std::string_view line) noexcept {
    if (!impl_) return;
    impl_->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->stream.put('\n');
    ++linesSinceFlush_;
}

void MmapJsonWriter::flushIfNeeded() noexcept {
    if (!impl_) return;
    if (linesSinceFlush_ >= flushBatchLines_) {
        impl_->stream.flush();
        linesSinceFlush_ = 0;
    }
}

