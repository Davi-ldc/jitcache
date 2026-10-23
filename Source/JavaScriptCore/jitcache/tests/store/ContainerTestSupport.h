#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/store/JITCachePlatform.h"
#include <span>
#include <stdint.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/SystemMalloc.h>
#include <wtf/Vector.h>

// What the container's tests share: little-endian fields written and read byte by byte, the CRC-32C, and buffers of
// exactly an input's size from the system allocator, over which ASan reports any read past the end. The names live in a
// namespace of their own, since names such as putField are generic.

namespace JSC::JITCache::Tests::ContainerTestSupport {

inline uint32_t crc32cOf(std::span<const uint8_t> bytes)
{
    return ~crc32cExtend(~0u, bytes);
}

// Little-endian fields, written and read byte by byte.
inline void putField(Vector<uint8_t>& bytes, size_t offset, uint64_t value, unsigned width)
{
    for (unsigned index = 0; index < width; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

inline void appendField(Vector<uint8_t>& bytes, uint64_t value, unsigned width)
{
    for (unsigned index = 0; index < width; ++index)
        bytes.append(static_cast<uint8_t>(value >> (8 * index)));
}

inline uint64_t getField(std::span<const uint8_t> bytes, size_t offset, unsigned width)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < width; ++index)
        value |= static_cast<uint64_t>(bytes[offset + index]) << (8 * index);
    return value;
}

// A buffer of exactly the input's size from the system allocator.
using ExactBuffer = MallocSpan<uint8_t, SystemMallocBase<uint8_t>>;

inline ExactBuffer exactCopy(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return { };
    auto copy = ExactBuffer::malloc(bytes.size());
    memcpySpan(copy.mutableSpan(), bytes);
    return copy;
}

} // namespace JSC::JITCache::Tests::ContainerTestSupport

#endif // ENABLE(JITCACHE_TWINS)
