#pragma once
#include <cassert>
#include <iostream>

template <class T>
class RingBuffer
{
public:
    explicit RingBuffer(unsigned size) : m_size(size), m_front(0), m_rear(0)
    {
        assert(size > 0);  // ✅ Prevent zero-size buffers
        m_data = new T[size];
    }

    ~RingBuffer()
    {
        if (m_data) {
            delete[] m_data;
            m_data = nullptr;
        }
    }

    inline bool isEmpty() const
    {
        return m_front == m_rear;
    }

    inline bool isFull() const
    {
        return (m_rear + 1) % m_size == m_front;  // ✅ Fixed off-by-one error
    }

    bool push(const T& value)
    {
        if (isFull())
        {
            return false;
        }
        m_data[m_rear] = value;
        m_rear = (m_rear + 1) % m_size;
        return true;
    }

    bool pop(T& value)
    {
        if (isEmpty())
        {
            return false;
        }
        value = m_data[m_front];
        m_front = (m_front + 1) % m_size;
        return true;
    }

    // ✅ Returns actual front element instead of an index
    inline T front() const
    {
        if (!isEmpty())
            return m_data[m_front];
        throw std::runtime_error("RingBuffer is empty!");
    }

    inline unsigned int size() const
    {
        return m_size;
    }

private:
    unsigned int m_size;
    unsigned int m_front;
    unsigned int m_rear;
    T* m_data;
};
