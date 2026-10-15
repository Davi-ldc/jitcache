#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/StdLibExtras.h>

// Reads and writes of fixed-offset fields in the byte spans that JITCache's sections and files hold. Every target
// JITCache builds for is little-endian, so a field's stored bytes are its native representation, and each access is a
// memcpy that works at any alignment. A field takes exactly sizeof(T) bytes: callers always name T, so the width never
// follows the type of the value passed. Each access cuts its field out with std::span::subspan, whose standard-library
// bounds assertion stops the process when the field does not lie inside the span; code that must refuse malformed
// bytes checks their size before it reads them. Every part of JITCache may include this header, and its functions touch
// only the bytes they are given, so any thread may call them.

namespace JSC::JITCache {

static_assert(std::endian::native == std::endian::little);

// T is any trivially copyable type: an integer, a float, a digest or a record such as a section header.
template<typename T>
inline T readField(std::span<const uint8_t> bytes, size_t offset)
{
    std::array<uint8_t, sizeof(T)> raw;
    memcpySpan(std::span { raw }, bytes.subspan(offset, sizeof(T)));
    return std::bit_cast<T>(raw);
}

template<typename T>
inline void writeField(std::span<uint8_t> bytes, size_t offset, const std::type_identity_t<T>& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

inline bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

} // namespace JSC::JITCache
