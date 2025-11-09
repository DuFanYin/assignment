#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <concepts>

/**
 * Lock-free SPSC (Single Producer Single Consumer) ring buffer
 * Based on Disruptor pattern for high-performance queue
 */
template<typename T>
requires std::copyable<T>
class RingBuffer {
public:
    constexpr static bool is_power_of_two(size_t n) noexcept {
        return n > 0 && (n & (n - 1)) == 0;
    }
    
    explicit RingBuffer(size_t capacity) 
        : capacity_(capacity)
        , mask_(capacity_ - 1)  // For power-of-2 sizes
        , buffer_(capacity_)
        , read_pos_(0)
        , write_pos_(0) {
        // Ensure capacity is power of 2
        if (!is_power_of_two(capacity_)) {
            throw std::invalid_argument("Ring buffer capacity must be power of 2");
        }
    }
    
    bool try_push(const T& item) {
        size_t current_write = write_pos_.load(std::memory_order_relaxed);
        size_t next_write = current_write + 1;
        
        // Check if buffer is full (leave 1 slot empty to distinguish full from empty)
        if ((next_write & mask_) == (read_pos_.load(std::memory_order_acquire) & mask_)) {
            return false;  // Buffer full
        }
        
        buffer_[current_write & mask_] = item;
        write_pos_.store(next_write, std::memory_order_release);
        // Wake up consumers waiting for data
        write_pos_.notify_one();
        return true;
    }
    
    bool try_pop(T& item) {
        size_t current_read = read_pos_.load(std::memory_order_relaxed);
        
        // Check if buffer is empty
        if (current_read == write_pos_.load(std::memory_order_acquire)) {
            return false;  // Buffer empty
        }
        
        item = buffer_[current_read & mask_];
        read_pos_.store(current_read + 1, std::memory_order_release);
        // Wake potential producers waiting for space
        read_pos_.notify_one();
        return true;
    }
    
    size_t size() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_acquire);
        if (w >= r) {
            return w - r;
        } else {
            return capacity_ - (r - w);
        }
    }
    
    bool empty() const {
        return read_pos_.load(std::memory_order_acquire) == 
               write_pos_.load(std::memory_order_acquire);
    }

    // C++20 blocking helpers
    void wait_for_data() const {
        // Wait until write_pos_ changes (i.e., new data is written)
        size_t observed = write_pos_.load(std::memory_order_acquire);
        write_pos_.wait(observed, std::memory_order_acquire);
    }

    void wait_for_space() const {
        // Wait until read_pos_ changes (i.e., consumer freed space)
        size_t observed = read_pos_.load(std::memory_order_acquire);
        read_pos_.wait(observed, std::memory_order_acquire);
    }

    void notify_all() {
        write_pos_.notify_all();
        read_pos_.notify_all();
    }

    // Blocking variants
    void push(const T& item) {
        while (true) {
            if (try_push(item)) {
                return;
            }
            wait_for_space();
        }
    }

    bool pop(T& item) {
        while (true) {
            if (try_pop(item)) {
                return true;
            }
            // If empty, wait for writers
            wait_for_data();
        }
    }
    
private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> read_pos_;
    char pad_[64 - sizeof(std::atomic<size_t>) > 0 ? 64 - sizeof(std::atomic<size_t>) : 0]{};
    alignas(64) std::atomic<size_t> write_pos_;
};



