#pragma once

#include <string>
#include <string_view>
#include <cstddef>

// Portable append-only JSON line writer.
// - On macOS/Linux: uses mmap with msync(MS_ASYNC) batching
// - Other platforms: falls back to buffered std::ofstream (append mode)
class MmapJsonWriter {
public:
    // Construct an append-only writer that keeps the file open for the lifetime
    // of the object. Throws std::runtime_error on critical initialization errors.
    explicit MmapJsonWriter(const std::string& filePath,
                            std::size_t initialCapacityBytes = 4 * 1024 * 1024,
                            std::size_t flushBatchLines = 100);

    ~MmapJsonWriter();

    // Append a single line (a trailing '\n' will be added). Never throws; on
    // failure it will attempt to recover (resize/remap) and otherwise drop the
    // write to keep the receiver running.
    void appendLine(std::string_view line) noexcept;

    // Trigger an async flush when enough lines have been appended since the last
    // flush. No-op if not needed. Never throws.
    void flushIfNeeded() noexcept;

    // Non-copyable, non-movable (file-backed resource)
    MmapJsonWriter(const MmapJsonWriter&) = delete;
    MmapJsonWriter& operator=(const MmapJsonWriter&) = delete;

private:
    // Common state
    std::string filePath_;
    std::size_t flushBatchLines_;
    std::size_t linesSinceFlush_;

    // Buffered writer implementation only (portable)
    class Impl;
    Impl* impl_;
};


