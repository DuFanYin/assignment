#include "util/ring_buffer.hpp"
#include "util/utils.hpp"

#include <utility>

template <typename T>
requires std::copyable<T>
RingBuffer<T>::RingBuffer()
    : capacity_(kCapacity)
    , mask_(capacity_ - 1)
    , buffer_(capacity_)
    , read_pos_(0)
    , write_pos_(0) {
    static_assert(is_power_of_two(kCapacity), "Capacity must be a power of 2");
}

template <typename T>
requires std::copyable<T>
bool RingBuffer<T>::try_push(const T& item) {
    size_t write = write_pos_.load(std::memory_order_relaxed);
    size_t next = write + 1;

    if ((next & mask_) == (read_pos_.load(std::memory_order_acquire) & mask_)) {
        return false;
    }

    buffer_[write & mask_] = item;
    write_pos_.store(next, std::memory_order_release);
    write_pos_.notify_one();
    return true;
}

template <typename T>
requires std::copyable<T>
bool RingBuffer<T>::try_pop(T& item) {
    size_t read = read_pos_.load(std::memory_order_relaxed);

    if (read == write_pos_.load(std::memory_order_acquire)) {
        return false;
    }

    item = buffer_[read & mask_];
    read_pos_.store(read + 1, std::memory_order_release);
    read_pos_.notify_one();
    return true;
}

template <typename T>
requires std::copyable<T>
void RingBuffer<T>::push(const T& item) {
    while (!try_push(item)) {
        wait_for_space();
    }
}

template <typename T>
requires std::copyable<T>
void RingBuffer<T>::pop(T& item) {
    while (!try_pop(item)) {
        wait_for_data();
    }
}

template <typename T>
requires std::copyable<T>
bool RingBuffer<T>::pop(T& item, const std::function<bool()>& should_exit) {
    while (true) {
        if (try_pop(item)) {
            return true;
        }
        if (should_exit && should_exit()) {
            return false;
        }

        size_t observed = write_pos_.load(std::memory_order_acquire);
        if (should_exit && should_exit()) {
            return false;
        }
        write_pos_.wait(observed, std::memory_order_acquire);
    }
}

template <typename T>
requires std::copyable<T>
size_t RingBuffer<T>::size() const {
    size_t w = write_pos_.load(std::memory_order_acquire);
    size_t r = read_pos_.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : (capacity_ - (r - w));
}

template <typename T>
requires std::copyable<T>
bool RingBuffer<T>::empty() const {
    return read_pos_.load(std::memory_order_acquire) ==
           write_pos_.load(std::memory_order_acquire);
}

template <typename T>
requires std::copyable<T>
void RingBuffer<T>::notify_all() {
    write_pos_.notify_all();
    read_pos_.notify_all();
}

template <typename T>
requires std::copyable<T>
constexpr bool RingBuffer<T>::is_power_of_two(size_t n) noexcept {
    return n > 0 && (n & (n - 1)) == 0;
}

template <typename T>
requires std::copyable<T>
void RingBuffer<T>::wait_for_data() const {
    size_t observed = write_pos_.load(std::memory_order_acquire);
    write_pos_.wait(observed, std::memory_order_acquire);
}

template <typename T>
requires std::copyable<T>
void RingBuffer<T>::wait_for_space() const {
    size_t observed = read_pos_.load(std::memory_order_acquire);
    read_pos_.wait(observed, std::memory_order_acquire);
}

template class RingBuffer<MboMessageWrapper>;


