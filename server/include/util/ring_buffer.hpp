#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <vector>

/**
 * Lock-free SPSC (Single Producer Single Consumer) RingBuffer.
 * Features:
 * - Fixed-size circular buffer (power of 2)
 * - try_push / try_pop: non-blocking
 * - push / pop: blocking with atomic::wait/notify
 * - pop(predicate): supports external exit flag
 */
template <typename T>
requires std::copyable<T>
class RingBuffer {
public:
    static constexpr size_t kCapacity = 65536;

    RingBuffer();

    // === Non-blocking APIs ===
    bool try_push(const T& item);
    bool try_pop(T& item);

    // === Blocking APIs ===
    void push(const T& item);
    void pop(T& item);
    bool pop(T& item, const std::function<bool()>& should_exit = {});

    // === Utilities ===
    size_t size() const;
    bool empty() const;
    void notify_all();

private:
    static constexpr bool is_power_of_two(size_t n) noexcept;
    void wait_for_data() const;
    void wait_for_space() const;

private:
    static constexpr size_t kCacheLineSize = 64;
    static constexpr size_t kPaddingSize =
        (kCacheLineSize > sizeof(std::atomic<size_t>))
            ? (kCacheLineSize - sizeof(std::atomic<size_t>))
            : 0;

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;

    // Prevent false sharing
    alignas(64) std::atomic<size_t> read_pos_;
    std::array<char, kPaddingSize> padding_{};
    alignas(64) std::atomic<size_t> write_pos_;
};