#include "Core/Allocator.h"
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace mf {

LinearAllocator::LinearAllocator(size_t capacity)
    : m_capacity(capacity), m_offset(0) {
#ifdef _MSC_VER
    m_buffer = static_cast<uint8_t*>(_aligned_malloc(capacity, 16));
#else
    m_buffer = static_cast<uint8_t*>(std::aligned_alloc(16, capacity));
#endif
}

LinearAllocator::~LinearAllocator() {
    if (m_buffer) {
#ifdef _MSC_VER
        _aligned_free(m_buffer);
#else
        std::free(m_buffer);
#endif
    }
}

uint8_t* LinearAllocator::allocate(size_t size, size_t alignment) {
    size_t mask = alignment - 1;
    size_t addr = (m_offset + mask) & ~mask;
    if (addr + size > m_capacity) return nullptr;
    m_offset = addr + size;
    return m_buffer + addr;
}

void LinearAllocator::reset() {
    m_offset = 0;
}

} // namespace mf
