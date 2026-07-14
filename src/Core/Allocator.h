#pragma once

#include <vector>
#include <memory>
#include <cstdint>

namespace mf {

// ------------------------------------------------------------------
// Linear bump allocator for transient mesh data
// ------------------------------------------------------------------
class LinearAllocator {
public:
    explicit LinearAllocator(size_t capacity);
    ~LinearAllocator();

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    uint8_t* allocate(size_t size, size_t alignment = 16);
    void reset();
    size_t used() const { return m_offset; }
    size_t capacity() const { return m_capacity; }

private:
    uint8_t* m_buffer = nullptr;
    size_t m_capacity = 0;
    size_t m_offset = 0;
};

// ------------------------------------------------------------------
// Object pool for reusable GPU batches
// ------------------------------------------------------------------
template<typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t initial = 256);
    T* acquire();
    void release(T* obj);
    size_t size() const { return m_pool.size(); }

private:
    std::vector<std::unique_ptr<T>> m_pool;
    std::vector<T*> m_free;
};

} // namespace mf
