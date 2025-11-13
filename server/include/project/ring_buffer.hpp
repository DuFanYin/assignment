#pragma once

#include <vector>
#include <atomic>
#include <concepts>

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

    RingBuffer()
        : capacity_(kCapacity),
          mask_(capacity_ - 1),
          buffer_(capacity_),
          read_pos_(0),
          write_pos_(0) {
        static_assert(is_power_of_two(kCapacity), "Capacity must be a power of 2");
    }

    // === Non-blocking APIs ===
    bool try_push(const T& item) {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t next = write + 1;

        // Leave 1 slot empty to distinguish full vs empty
        if ((next & mask_) == (read_pos_.load(std::memory_order_acquire) & mask_)) {
            return false;  // Buffer full
        }

        buffer_[write & mask_] = item;
        write_pos_.store(next, std::memory_order_release);
        write_pos_.notify_one();  // Wake consumer
        return true;
    }

    bool try_pop(T& item) {
        size_t read = read_pos_.load(std::memory_order_relaxed);

        if (read == write_pos_.load(std::memory_order_acquire)) {
            return false;  // Buffer empty
        }

        item = buffer_[read & mask_];
        read_pos_.store(read + 1, std::memory_order_release);
        read_pos_.notify_one();  // Wake producer
        return true;
    }

    // === Blocking APIs ===
    void push(const T& item) {
        while (!try_push(item)) {
            wait_for_space();
        }
    }

    void pop(T& item) {
        while (!try_pop(item)) {
            wait_for_data();
        }
    }

    // Blocking pop with exit predicate
    template <typename Predicate>
    bool pop(T& item, Predicate&& should_exit) {
        while (true) {
            if (try_pop(item)) return true;
            if (should_exit()) return false;

            size_t observed = write_pos_.load(std::memory_order_acquire);
            if (should_exit()) return false;
            write_pos_.wait(observed, std::memory_order_acquire);
        }
    }

    // === Utilities ===
    size_t size() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (capacity_ - (r - w));
    }

    bool empty() const {
        return read_pos_.load(std::memory_order_acquire) ==
               write_pos_.load(std::memory_order_acquire);
    }

    void notify_all() {
        write_pos_.notify_all();
        read_pos_.notify_all();
    }

private:
    constexpr static bool is_power_of_two(size_t n) noexcept {
        return n > 0 && (n & (n - 1)) == 0;
    }

    void wait_for_data() const {
        size_t observed = write_pos_.load(std::memory_order_acquire);
        write_pos_.wait(observed, std::memory_order_acquire);
    }

    void wait_for_space() const {
        size_t observed = read_pos_.load(std::memory_order_acquire);
        read_pos_.wait(observed, std::memory_order_acquire);
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;

    // Prevent false sharing
    alignas(64) std::atomic<size_t> read_pos_;
    char pad_[64 - sizeof(std::atomic<size_t>) > 0 ? 64 - sizeof(std::atomic<size_t>) : 0]{};
    alignas(64) std::atomic<size_t> write_pos_;
};