#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstring>

/**
 * Lock-free SPSC (Single Producer Single Consumer) ring buffer
 * Based on Disruptor pattern for high-performance queue
 */
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) 
        : capacity_(capacity)
        , mask_(capacity_ - 1)  // For power-of-2 sizes
        , buffer_(capacity_)
        , read_pos_(0)
        , write_pos_(0) {
        // Ensure capacity is power of 2
        if ((capacity_ & (capacity_ - 1)) != 0) {
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
    
private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    std::atomic<size_t> read_pos_;
    std::atomic<size_t> write_pos_;
};

