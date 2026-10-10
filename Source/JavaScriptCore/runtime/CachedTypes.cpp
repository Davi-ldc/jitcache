/*
 * Copyright (C) 2019-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CachedTypes.h"
#include <wtf/Deque.h>
#include <wtf/Function.h>
#if CPU(X86_64)
#include <cpuid.h>
#endif

#include "BaselineJITCode.h"
#include "BuiltinNames.h"
#include "BytecodeCacheError.h"
#include "BytecodeLivenessAnalysis.h"
#include "JITCacheSHA256.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSTemplateObjectDescriptor.h"
#include "ScopedArgumentsTable.h"
#include "SourceCodeKey.h"
#include "SourceProvider.h"
#include "SymbolTableInlines.h"
#include "UCBRequests.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedMetadataTableInlines.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include <ranges>
#include <wtf/CheckedArithmetic.h>
#include <wtf/FileHandle.h>
#include <wtf/HashSet.h>
#include <wtf/InlineMap.h>
#include <wtf/MallocSpan.h>
#include <wtf/Packed.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UUID.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringHasher.h>

#if ENABLE(JITCACHE_TWINS)
#include "CodeCache.h"
#include "JSLock.h"
#include "ParserError.h"
#include "StrongInlines.h"
#include <wtf/text/MakeString.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// A payload written on one platform is read in place on another, so the records below must be laid out identically under
// the Itanium and MSVC C++ ABIs. Their bit-fields are the likeliest thing to break that: make the compiler refuse any
// bit-field in this file that MSVC would pack differently.
#if defined(__has_warning)
#if __has_warning("-Wms-bitfield-padding")
#pragma clang diagnostic error "-Wms-bitfield-padding"
#endif
#endif

// Everything placed in a payload must have no padding bytes and no unused bit-field bits on the ABI compiling this. The
// C++ ABIs a payload moves between all place bases and fields in declaration order and differ only in where they pad, so a
// type that is padding-free under each of them has the same field offsets under all of them.
// (Hence the m_unused members below: padding, spelled out.) double only fails the trait because +0/-0 and NaNs have
// several representations; its layout is IEEE-754 binary64 everywhere.
template<typename T> concept PayloadType = std::has_unique_object_representations_v<T> || std::is_same_v<T, double>;

namespace JSC {

bool Decoder::canBorrowPayload() const
{
    // JITCache: nothing decoded from a core points into the body file (SPEC-ucb.codec.md, E7).
    if (m_purpose == Purpose::JITCacheCore)
        return false;
#if USE(BUN_JSC_ADDITIONS)
    return Options::useBorrowedBytecodeFromCache() && m_cachedBytecode->payloadIsPersistent();
#else
    return false;
#endif
}

// Scalars of the per-function records are written as a LEB128 tail right after the fixed part of the record: most of them
// are small or zero in almost every function, and they are read exactly once, into the object being constructed.
class VarintWriter {
public:
    void u32(uint32_t v)
    {
        while (v >= 0x80) {
            m_bytes.append(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        m_bytes.append(static_cast<uint8_t>(v));
    }
    void i32(int32_t v) { u32((static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)); }
    void u8(uint8_t v) { m_bytes.append(v); }
    size_t size() const { return m_bytes.size(); }
    void copyTo(uint8_t* out) const { memcpy(out, m_bytes.span().data(), m_bytes.size()); }
    // JITCache: the buffer a long tail spills out of its inline capacity, which an encode charges (SPEC-ucb.codec.md, E8).
    size_t heapBytes() const { return m_bytes.capacity() > 128 ? m_bytes.capacity() : 0; }

private:
    Vector<uint8_t, 128> m_bytes;
};

class VarintReader {
public:
    // `end` bounds the read when the bytes have not been checksummed yet; past it every read yields 0 and overran() is set.
    explicit VarintReader(const uint8_t* p, const uint8_t* end = nullptr)
        : m_p(p)
        , m_end(end)
    {
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t b = u8();
            v |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
            if (shift >= 28) {
                m_overran = true;
                return 0;
            }
        }
    }
    int32_t i32()
    {
        uint32_t v = u32();
        return static_cast<int32_t>((v >> 1) ^ -(v & 1));
    }
    uint8_t u8()
    {
        if (m_end && m_p >= m_end) {
            m_overran = true;
            return 0;
        }
        return *m_p++;
    }
    bool overran() const { return m_overran; }
    const uint8_t* position() const { return m_p; }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_overran { false };
};

// CRC-32C of the bytes one code-block decode reads, so a truncated or corrupted payload falls back to generating that
// function from source instead of being trusted. Hardware where the ISA guarantees it, a table elsewhere.
static uint32_t crc32cSoftware(uint32_t crc, std::span<const uint8_t> bytes)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t { };
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (uint8_t byte : bytes)
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return crc;
}

#if CPU(X86_64)
__attribute__((target("sse4.2"))) static uint32_t crc32cHardware(uint32_t crc, std::span<const uint8_t> bytes)
{
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    uint64_t c = crc;
    for (; n >= 8; n -= 8, p += 8)
        c = __builtin_ia32_crc32di(c, WTF::unalignedLoad<uint64_t>(p));
    for (; n; --n, ++p)
        c = __builtin_ia32_crc32qi(static_cast<uint32_t>(c), *p);
    return static_cast<uint32_t>(c);
}
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
static uint32_t crc32cHardware(uint32_t crc, std::span<const uint8_t> bytes)
{
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    for (; n >= 8; n -= 8, p += 8)
        crc = __builtin_arm_crc32cd(crc, WTF::unalignedLoad<uint64_t>(p));
    for (; n; --n, ++p)
        crc = __builtin_arm_crc32cb(crc, *p);
    return crc;
}
#endif

// External linkage: jitcache/JITCachePlatform.h declares it, and JITCache's artifact files are checksummed with it too.
uint32_t crc32c(uint32_t crc, std::span<const uint8_t> bytes)
{
#if CPU(X86_64)
    static const bool hardware = [] {
        // cpuid directly: __builtin_cpu_supports needs compiler-rt's __cpu_model, which not every link provides.
        unsigned eax, ebx, ecx = 0, edx;
        return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_SSE4_2);
    }();
    if (hardware)
        return crc32cHardware(crc, bytes);
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
    return crc32cHardware(crc, bytes);
#endif
    return crc32cSoftware(crc, bytes);
}

AtomStringImpl* Decoder::atomForOrdinal(uint32_t ordinal) const
{
    return ordinal < m_atomsByOrdinal.size() ? m_atomsByOrdinal[ordinal] : nullptr;
}

void Decoder::setAtomForOrdinal(uint32_t ordinal, AtomStringImpl& atom)
{
    if (ordinal >= m_atomsByOrdinal.size()) {
        // Payloads are far below 2^32 bytes and every numbered string is a 12+ byte record, so this is bounded by the payload.
        RELEASE_ASSERT(ordinal < m_cachedBytecode->size());
        size_t oldSize = m_atomsByOrdinal.size();
        m_atomsByOrdinal.grow(std::max<size_t>(ordinal + 1, oldSize * 2));
        std::fill(m_atomsByOrdinal.begin() + oldSize, m_atomsByOrdinal.end(), nullptr); // Vector::grow leaves pointers uninitialized
    }
    ASSERT(!m_atomsByOrdinal[ordinal]);
    atom.ref();
    m_atomsByOrdinal[ordinal] = &atom;
}

// 1- and 2-character inline strings are the bulk of minified identifiers: length 1 is SmallStrings' single-character reps; length 2 hits one lazy 65536-entry table on the VM (shared by every Decoder — one 512 KB slab, not one per retained Decoder); length 3 goes to the atom table each time.
Ref<AtomStringImpl> Decoder::atomForInlineString(VM& vm, std::span<const uint8_t, 4> slot)
{
    static_assert(std::endian::native == std::endian::little, "inline string slots are written as a little-endian word");
    unsigned length = (slot[0] >> 2) & 3;
    std::span<const Latin1Character> characters = slot.subspan(1).first(length);
    if (length == 1)
        return vm.smallStrings.singleCharacterStringRep(characters[0]);
    if (length == 2) {
        AtomStringImpl*& entry = vm.ensureCachedBytecodeTwoCharacterAtoms()[characters[0] | characters[1] << 8];
        if (entry) [[likely]]
            return *entry;
        Ref<AtomStringImpl> atom = AtomStringImpl::add(characters).releaseNonNull();
        atom->ref();
        entry = atom.ptr();
        return atom;
    }
    return AtomStringImpl::add(characters).releaseNonNull();
}

ALWAYS_INLINE DecoderStringTable& Decoder::externalStrings()
{
    if (!m_externalStrings) [[unlikely]] {
        m_externalStrings = m_vm.clientData ? m_vm.clientData->decoderStringTable() : nullptr;
        RELEASE_ASSERT_WITH_MESSAGE(m_externalStrings, "bytecode payload uses an external string table but the embedder did not provide one");
    }
    return *m_externalStrings;
}

Ref<AtomStringImpl> Decoder::atomForExternalString(uint32_t ordinal)
{
    return externalStrings().atomFor(m_vm, ordinal);
}

JSString* Decoder::jsStringForExternalString(uint32_t ordinal)
{
    return externalStrings().jsStringFor(m_vm, ordinal);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(EncoderStringTable);
WTF_MAKE_TZONE_ALLOCATED_IMPL(DecoderStringTable);

EncoderStringTable::~EncoderStringTable() = default;

uint32_t EncoderStringTable::ordinalFor(const StringImpl& string)
{
    ASSERT(!string.isSymbol() && string.length());
    auto result = m_ordinals.add(String { const_cast<StringImpl*>(&string) }, static_cast<uint32_t>(m_strings.size()));
    if (result.isNewEntry)
        m_strings.append(const_cast<StringImpl&>(string));
    return result.iterator->value;
}

uint32_t EncoderStringTable::slotFor(const StringImpl& string)
{
    RELEASE_ASSERT(!string.isSymbol()); // a slot names text; a symbol has no slot form (CachedPtr writes a record for it)
    if (!string.length())
        return VariableLengthObjectBase::emptySentinel;
    if (std::optional<uint32_t> packed = VariableLengthObjectBase::packInlineString(string))
        return *packed;
    uint32_t ordinal = ordinalFor(string);
    RELEASE_ASSERT(ordinal <= maxOrdinal);
    return VariableLengthObjectBase::externalStringTag | ordinal << 2;
}

// [u32 count][u32 offsets[count]][records: {u32 length|is8Bit<<31, u32 hash, chars, pad-to-4}...]; offsets are from the start of the blob.
Vector<uint8_t> EncoderStringTable::serialize() const
{
    Vector<uint8_t> out;
    uint32_t count = static_cast<uint32_t>(m_strings.size());
    size_t header = sizeof(uint32_t) * (1 + static_cast<size_t>(count));
    size_t body = 0;
    for (auto& s : m_strings)
        body += roundUpToMultipleOf<4>(2 * sizeof(uint32_t) + s->length() * (s->is8Bit() ? sizeof(Latin1Character) : sizeof(char16_t)));
    out.grow(header + body);
    std::memset(out.mutableSpan().data(), 0, out.size());
    uint32_t* words = std::bit_cast<uint32_t*>(out.mutableSpan().data());
    words[0] = count;
    size_t offset = header;
    for (uint32_t i = 0; i < count; ++i) {
        words[1 + i] = static_cast<uint32_t>(offset);
        const StringImpl& s = m_strings[i].get();
        uint32_t* record = std::bit_cast<uint32_t*>(out.mutableSpan().data() + offset);
        record[0] = s.length() | (s.is8Bit() ? 1u << 31 : 0);
        record[1] = s.hash();
        if (s.is8Bit())
            std::memcpy(record + 2, s.span8().data(), s.length());
        else
            std::memcpy(record + 2, s.span16().data(), s.length() * sizeof(char16_t));
        offset += roundUpToMultipleOf<4>(2 * sizeof(uint32_t) + s.length() * (s.is8Bit() ? sizeof(Latin1Character) : sizeof(char16_t)));
    }
    ASSERT(offset == out.size());
    return out;
}

DecoderStringTable::DecoderStringTable(std::span<const uint8_t> bytes)
    : m_bytes(bytes)
{
    RELEASE_ASSERT(bytes.size() >= sizeof(uint32_t) && !(std::bit_cast<uintptr_t>(bytes.data()) % alignof(uint32_t)));
    m_count = *std::bit_cast<const uint32_t*>(bytes.data());
    RELEASE_ASSERT(m_count <= (bytes.size() - sizeof(uint32_t)) / sizeof(uint32_t), m_count, bytes.size());
    if (m_count) {
        m_slotsReservation = roundUpToMultipleOf(WTF::pageSize(), static_cast<size_t>(m_count) * sizeof(uintptr_t));
        m_slots = static_cast<uintptr_t*>(OSAllocator::reserveAndCommit(m_slotsReservation, OSAllocator::FastMallocPages));
    }
}

DecoderStringTable::~DecoderStringTable()
{
    // One per VM: a Worker that exits must give back the references it took on its thread's atoms.
    for (uint32_t i = 0; i < m_count; ++i) {
        if (m_slots[i] && !isCell(m_slots[i]))
            impl(m_slots[i])->deref();
    }
    if (m_slots)
        OSAllocator::decommitAndRelease(m_slots, m_slotsReservation);
}

StringImpl* DecoderStringTable::impl(uintptr_t slot)
{
    if (isCell(slot))
        return cell(slot)->tryGetValueImpl(); // never a rope: jsStringFor made it from a resolved StringImpl
    return std::bit_cast<StringImpl*>(slot);
}

// The slot's StringImpl, decoding it (as a plain, non-atom string) if the slot is still empty.
StringImpl* DecoderStringTable::ensureImpl(uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    uintptr_t& slot = m_slots[ordinal];
    if (slot)
        return impl(slot);
    Record r = record(ordinal);
    RefPtr<StringImpl> string;
    if (r.is8Bit) {
        std::span<const Latin1Character> chars { std::bit_cast<const Latin1Character*>(r.characters), r.length };
        string = r.length >= 48 ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
    } else {
        std::span<const char16_t> chars { std::bit_cast<const char16_t*>(r.characters), r.length };
        string = r.length >= 48 ? StringImpl::createWithoutCopying(chars) : StringImpl::create(chars);
    }
    slot = std::bit_cast<uintptr_t>(string.leakRef()); // the table's +1
    return impl(slot);
}

// The blob comes from an executable users sometimes edit; never read outside it.
DecoderStringTable::Record DecoderStringTable::record(uint32_t ordinal) const
{
    RELEASE_ASSERT(ordinal < m_count);
    const uint32_t* offsets = std::bit_cast<const uint32_t*>(m_bytes.data() + sizeof(uint32_t));
    size_t offset = offsets[ordinal];
    RELEASE_ASSERT(!(offset % 4) && offset <= m_bytes.size() && m_bytes.size() - offset >= 2 * sizeof(uint32_t), offset, m_bytes.size());
    const uint32_t* header = std::bit_cast<const uint32_t*>(m_bytes.data() + offset);
    Record result;
    result.length = header[0] & 0x7fffffffu;
    result.is8Bit = header[0] >> 31;
    result.hash = header[1];
    result.characters = std::bit_cast<const uint8_t*>(header + 2);
    size_t byteLength = static_cast<size_t>(result.length) * (result.is8Bit ? sizeof(Latin1Character) : sizeof(char16_t));
    RELEASE_ASSERT(byteLength <= m_bytes.size() - offset - 2 * sizeof(uint32_t), ordinal, result.length, m_bytes.size());
    return result;
}

template<typename CharacterType>
static Ref<AtomStringImpl> atomize(std::span<const CharacterType> characters, uint32_t hash)
{
    // Same threshold as CachedUniquedStringImplBase::minimumLengthToAliasPayload: long strings alias the (persistent) blob.
    if (characters.size() >= 48)
        return AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(characters) }).releaseNonNull();
    WTF::HashTranslatorCharBuffer<CharacterType> hashed { characters, hash };
    return AtomStringImpl::add(hashed).releaseNonNull();
}

// A slot holds the one StringImpl this VM uses for that string: an atom once an identifier has asked for it, or the
// plain StringImpl a string constant made first (which atomFor then promotes or replaces).
Ref<AtomStringImpl> DecoderStringTable::atomFor(VM& vm, uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    uintptr_t& slot = m_slots[ordinal];
    if (slot) [[likely]] {
        StringImpl* existing = impl(slot);
        if (existing->isAtom()) [[likely]]
            return *static_cast<AtomStringImpl*>(existing);
        Ref<AtomStringImpl> atom = AtomStringImpl::add(existing).releaseNonNull(); // makes `existing` the atom unless one already exists
        if (atom.ptr() != existing) {
            if (isCell(slot))
                cell(slot)->swapToAtomString(vm, RefPtr<AtomStringImpl> { atom.ptr() });
            else {
                atom->ref();
                slot = std::bit_cast<uintptr_t>(static_cast<StringImpl*>(atom.ptr()));
                existing->deref();
            }
        }
        return atom;
    }
    Record r = record(ordinal);
    Ref<AtomStringImpl> atom = r.is8Bit
        ? atomize(std::span { std::bit_cast<const Latin1Character*>(r.characters), r.length }, r.hash)
        : atomize(std::span { std::bit_cast<const char16_t*>(r.characters), r.length }, r.hash);
    atom->ref();
    slot = std::bit_cast<uintptr_t>(static_cast<StringImpl*>(atom.ptr()));
    return atom;
}

RefPtr<AtomStringImpl> DecoderStringTable::atomForSlot(VM& vm, uint32_t slot)
{
    if (slot == VariableLengthObjectBase::emptySentinel)
        return emptyAtom().impl();
    switch (slot & VariableLengthObjectBase::inlineStringTagMask) {
    case VariableLengthObjectBase::inlineStringTag: {
        if (!((slot >> 2) & 3))
            return nullptr;
        return Decoder::atomForInlineString(vm, asByteSpan<uint32_t, sizeof(uint32_t)>(slot));
    }
    case VariableLengthObjectBase::externalStringTag:
        if (slot >> 2 >= m_count)
            return nullptr;
        return atomFor(vm, slot >> 2);
    default:
        return nullptr;
    }
}

JSString* DecoderStringTable::jsStringFor(VM& vm, uint32_t ordinal)
{
    RELEASE_ASSERT(ordinal < m_count);
    uintptr_t& slot = m_slots[ordinal];
    if (isCell(slot))
        return cell(slot);
    if (!slot) {
        Record r = record(ordinal);
        if (r.length == 1) {
            char16_t c = r.is8Bit ? *r.characters : *std::bit_cast<const char16_t*>(r.characters);
            if (c <= maxSingleCharacterString)
                return vm.smallStrings.singleCharacterString(c); // already shared VM-wide; leave the slot empty
        }
    }
    // The cell takes over the table's reference; the impl's bytes belong to the table (or the executable), not the GC heap.
    JSString* string = JSString::createHasOtherOwner(vm, adoptRef(*ensureImpl(ordinal)));
    slot = std::bit_cast<uintptr_t>(string) | cellTag;
    Locker locker { m_cellsLock };
    m_cellOrdinals.append(ordinal);
    return string;
}

template<typename Visitor>
void DecoderStringTable::visitStrongReferences(Visitor& visitor, CollectionScope scope)
{
    Locker locker { m_cellsLock };
    // Cells visited in an earlier cycle are old and stay marked (sticky mark bits); an eden collection only needs the
    // ones created since. The constraint can run more than once per cycle, so the cursor only advances, never resets,
    // within a cycle.
    size_t from = scope == CollectionScope::Full && !m_visitedThisCycle ? 0 : m_visitedCount;
    for (size_t i = from; i < m_cellOrdinals.size(); ++i)
        visitor.appendUnbarriered(cell(m_slots[m_cellOrdinals[i]]));
    m_visitedCount = m_cellOrdinals.size();
    m_visitedThisCycle = true;
}

void DecoderStringTable::didFinishCollection()
{
    Locker locker { m_cellsLock };
    m_visitedThisCycle = false;
}

template void DecoderStringTable::visitStrongReferences(AbstractSlotVisitor&, CollectionScope);
template void DecoderStringTable::visitStrongReferences(SlotVisitor&, CollectionScope);

std::span<const uint8_t> Decoder::payloadSpan() const
{
    return m_cachedBytecode->span();
}

bool Decoder::payloadContains(const void* start, size_t size) const
{
    auto payload = m_cachedBytecode->span();
    auto* begin = static_cast<const uint8_t*>(start);
    return begin >= payload.data() && size <= payload.size() && begin + size <= payload.data() + payload.size();
}

bool Decoder::verifiesChecksums() const
{
#if USE(BUN_JSC_ADDITIONS)
    // A persistent payload is a section of the executable itself: corruption there means the program is already broken, and code signing already covers it. Checksums guard separate on-disk cache files.
    return !m_cachedBytecode->payloadIsPersistent() && Options::verifyBytecodeCacheChecksums();
#else
    return true;
#endif
}

bool Decoder::regionChecksumMatches(const void* start, uint32_t size, const uint32_t* storedChecksum, std::span<const std::span<const uint8_t>> externalArrays) const
{
#if USE(BUN_JSC_ADDITIONS)
    if (!verifiesChecksums())
        return true;
#endif
    auto* begin = static_cast<const uint8_t*>(start);
    auto* hole = reinterpret_cast<const uint8_t*>(storedChecksum);
    if (!payloadContains(start, size) || hole < begin || hole + 4 > begin + size)
        return false; // includes a stored size too small to cover the record that holds the checksum
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, std::span { begin, hole });
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, std::span { hole + 4, begin + size });
    for (auto external : externalArrays) {
        if (!payloadContains(external.data(), external.size()))
            return false;
        crc = crc32c(crc, external);
    }
    uint32_t stored;
    memcpy(&stored, storedChecksum, sizeof(stored));
    if (~crc == stored)
        return true;
    dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] code block checksum mismatch; regenerating from source");
    return false;
}

namespace Yarr {
enum class Flags : uint16_t;
}

template <typename T, typename = void>
struct SourceTypeImpl {
    using type = T;
};

template<typename T>
struct SourceTypeImpl<T, std::enable_if_t<!std::is_fundamental<T>::value && !std::is_same<typename T::SourceType_, void>::value>> {
    using type = typename T::SourceType_;

};

template<typename T>
using SourceType = typename SourceTypeImpl<T>::type;

// Fixed rather than the host's alignof(std::max_align_t) / pageSize(): both decide where padding goes, and both vary by platform.
static constexpr size_t encoderMaxAlignment = 8;
static constexpr size_t encoderMinPageSize = 4 * KB;

// JITCache: the bytes a vector holds on the heap, which a JITCacheCore encode charges (SPEC-ucb.codec.md, E8).
template<typename T, size_t inlineCapacity, typename OverflowHandler, size_t minCapacity, typename Malloc>
static size_t encoderHeapBytes(const Vector<T, inlineCapacity, OverflowHandler, minCapacity, Malloc>& vector)
{
    return vector.capacity() > inlineCapacity ? vector.capacity() * sizeof(T) : 0;
}

class Encoder {
    WTF_MAKE_NONCOPYABLE(Encoder);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    class Allocation {
        friend class Encoder;

    public:
        uint8_t* NODELETE buffer() const { return m_buffer; }
        ptrdiff_t NODELETE offset() const { return m_offset; }

    private:
        Allocation(uint8_t* buffer, ptrdiff_t offset)
            : m_buffer(buffer)
            , m_offset(offset)
        {
        }

        uint8_t* m_buffer;
        ptrdiff_t m_offset;
    };

    // A payload that gets appended to another one (CachedBytecode::addFunctionUpdate) is read by the same Decoder as its
    // base, so it leaves its strings unnumbered rather than collide with numbers the base already handed out.
    enum class NumberStrings : bool { No, Yes };
    // JITCache: the bytecode cache's own payloads, or a JITCache core (SPEC-ucb.codec.md, section 4).
    using Purpose = Decoder::Purpose;

    // A JITCacheCore encoder writes no external string, no checksum and no updatable field, and numbers its strings within
    // its own payload. `budget` is charged for everything the encode allocates (E8); `startChain` is the TDZ chain that a
    // start record stands for (E14).
    Encoder(VM& vm, FileSystem::FileHandle& fileHandle, NumberStrings numberStrings = NumberStrings::Yes, EncoderStringTable* externalStrings = nullptr, BytecodeCacheChecksums checksums = BytecodeCacheChecksums::Yes, BytecodeCacheUpdatable updatable = BytecodeCacheUpdatable::Yes, Purpose purpose = Purpose::BytecodeCache, CoreEncodingBudget* budget = nullptr, const TDZEnvironmentLink* startChain = nullptr)
        : m_vm(vm)
        , m_fileHandle(fileHandle)
        , m_baseOffset(0)
        , m_currentPage(nullptr)
        , m_externalStrings(externalStrings)
        , m_numberStrings(numberStrings == NumberStrings::Yes)
        , m_updatable(updatable == BytecodeCacheUpdatable::Yes)
        , m_checksums(m_updatable || checksums == BytecodeCacheChecksums::Yes)
        , m_purpose(purpose)
        , m_budget(budget)
        , m_startChain(startChain)
    {
        ASSERT(m_purpose == Purpose::BytecodeCache || (!m_externalStrings && !m_checksums && m_numberStrings));
        ASSERT(m_purpose == Purpose::JITCacheCore || (!m_budget && !m_startChain));
        allocateNewPage();
    }

    Purpose purpose() const { return m_purpose; }
    bool isCore() const { return m_purpose == Purpose::JITCacheCore; }
    const TDZEnvironmentLink* startChain() const { return m_startChain; }

    // Charges (SPEC-ucb.codec.md, E8). Without a budget both calls do nothing. A refused charge makes the refusal sticky
    // and the allocation happens anyway, since an encode cannot unwind inside a record; every later charge counts as
    // refused, so nothing more is charged, and the owner releases chargedBytes() once the encoder is gone.
    bool budgetRefused() const { return m_budgetRefused; }
    size_t chargedBytes() const { return m_chargedBytes; }
    void chargeGrowth(size_t bytes)
    {
        if (!m_budget || !bytes || m_budgetRefused)
            return;
        if (!m_budget->charge(bytes)) {
            m_budgetRefused = true;
            return;
        }
        m_chargedBytes += bytes;
    }
    void releaseGrowth(size_t bytes)
    {
        // After a refusal a charge made before it stays held until the owner releases everything at once.
        if (!m_budget || !bytes || m_budgetRefused)
            return;
        ASSERT(bytes <= m_chargedBytes);
        m_budget->release(bytes);
        m_chargedBytes -= bytes;
    }
    // A table of the encoder's grew from `capacityBefore` to `capacityAfter` entries of `entrySize` bytes.
    void chargeTableGrowth(size_t capacityBefore, size_t capacityAfter, size_t entrySize)
    {
        if (capacityAfter > capacityBefore)
            chargeGrowth((capacityAfter - capacityBefore) * entrySize);
    }

    EncoderStringTable* NODELETE externalStrings() { return m_externalStrings; }

    // Every function nested in a class with private names carries a copy of the class's private-name environment; the
    // encoded entries are written once per distinct environment and shared.
    struct SharedPrivateNameEnvironment {
        unsigned hash;
        Vector<std::pair<const UniquedStringImpl*, uint16_t>> entries;
        ptrdiff_t elements;
    };
    std::optional<ptrdiff_t> sharedPrivateNameEnvironment(unsigned hash, const Vector<std::pair<const UniquedStringImpl*, uint16_t>>& entries) const
    {
        for (const auto& shared : m_sharedPrivateNameEnvironments) {
            // Pair by pair: Vector's operator== would memcmp the pairs' padding too.
            if (shared.hash == hash && std::ranges::equal(shared.entries, entries))
                return shared.elements;
        }
        return std::nullopt;
    }
    // `entries` keeps whatever charge its buffer already carries: the table now owns the buffer.
    void addSharedPrivateNameEnvironment(unsigned hash, Vector<std::pair<const UniquedStringImpl*, uint16_t>>&& entries, ptrdiff_t elements)
    {
        size_t capacity = m_sharedPrivateNameEnvironments.capacity();
        m_sharedPrivateNameEnvironments.append({ hash, WTF::move(entries), elements });
        chargeTableGrowth(capacity, m_sharedPrivateNameEnvironments.capacity(), sizeof(SharedPrivateNameEnvironment));
    }

    bool updatable() const { return m_updatable; }
    bool checksums() const { return m_checksums; }

    VM& vm() { return m_vm; }

    Allocation malloc(unsigned size, size_t alignment)
    {
        RELEASE_ASSERT(size);
        ptrdiff_t offset;
        if (m_currentPage->malloc(size, alignment, offset))
            return Allocation { m_currentPage->buffer() + offset, m_baseOffset + offset };
        allocateNewPage(size);
        return malloc(size, alignment);
    }

    template<PayloadType T, typename... Args>
    T* malloc(Args&&... args)
    {
        return new (malloc(sizeof(T), alignof(T)).buffer()) T(std::forward<Args>(args)...);
    }

    template<typename T, typename SourceArg>
    T* mallocFor(const SourceArg& source)
    {
        size_t tail = 0;
        if constexpr (requires { T::tailSize(*this, source); })
            tail = T::tailSize(*this, source);
        else if constexpr (requires { T::tailSize(source); })
            tail = T::tailSize(source);
        static_assert(PayloadType<T>);
        return new (malloc(sizeof(T) + tail, alignof(T)).buffer()) T();
    }

    ptrdiff_t currentOffset() const { return m_baseOffset + m_currentPage->size(); }

    // For an allocation whose size depends on where it lands: a record that stores its own offset as a varint.
    template<typename SizeAt>
    Allocation mallocPlaced(size_t alignment, const SizeAt& sizeAt)
    {
        ptrdiff_t offset = m_baseOffset + roundUpToMultipleOf(static_cast<ptrdiff_t>(alignment), static_cast<ptrdiff_t>(m_currentPage->size()));
        unsigned size = sizeAt(offset);
        ptrdiff_t pageOffset;
        if (!m_currentPage->malloc(size, alignment, pageOffset)) {
            // A fresh page starts max-aligned, so the allocation lands at its base.
            offset = m_baseOffset + roundUpToMultipleOf(static_cast<ptrdiff_t>(encoderMaxAlignment), static_cast<ptrdiff_t>(m_currentPage->size()));
            size = sizeAt(offset);
            allocateNewPage(size);
            bool fits = m_currentPage->malloc(size, alignment, pageOffset);
            RELEASE_ASSERT(fits);
        }
        RELEASE_ASSERT(m_baseOffset + pageOffset == offset);
        return Allocation { m_currentPage->buffer() + pageOffset, offset };
    }

    // CRC-32C of [offset, offset + size) as it will appear in the payload, with the 4 bytes at `hole` read as zero
    // (that is where the checksum itself is stored).
    uint32_t checksumOfRange(ptrdiff_t offset, size_t size, ptrdiff_t hole)
    {
        uint32_t crc = ~0u;
        ptrdiff_t baseOffset = 0;
        ptrdiff_t end = offset + size;
        for (const auto& page : m_pages) {
            ptrdiff_t pageEnd = baseOffset + page.size();
            ptrdiff_t from = std::max(offset, baseOffset);
            ptrdiff_t to = std::min(end, pageEnd);
            for (ptrdiff_t cursor = from; cursor < to;) {
                ptrdiff_t stop = to;
                if (cursor < hole)
                    stop = std::min(stop, hole);
                else if (cursor < hole + 4) {
                    static const uint8_t zeros[4] = { };
                    ptrdiff_t skip = std::min<ptrdiff_t>(hole + 4, to) - cursor;
                    crc = crc32c(crc, std::span { zeros, static_cast<size_t>(skip) });
                    cursor += skip;
                    continue;
                }
                crc = crc32c(crc, page.span().subspan(cursor - baseOffset, stop - cursor));
                cursor = stop;
            }
            baseOffset = pageEnd;
            if (baseOffset >= end)
                break;
        }
        return ~crc;
    }

    std::span<const uint8_t> bytesAt(ptrdiff_t offset, size_t size) { return mutableBytesAt(offset, size); }
    std::span<uint8_t> mutableBytesAt(ptrdiff_t offset, size_t size)
    {
        ptrdiff_t baseOffset = 0;
        for (auto& page : m_pages) {
            if (offset - baseOffset < static_cast<ptrdiff_t>(page.size()))
                return page.mutableSpan().subspan(offset - baseOffset, size);
            baseOffset += page.size();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ptrdiff_t offsetOf(const void* address)
    {
        ptrdiff_t offset;
        ptrdiff_t baseOffset = 0;
        for (const auto& page : m_pages) {
            if (page.getOffset(address, offset))
                return baseOffset + offset;
            baseOffset += page.size();
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }

    void cachePtr(const void* ptr, ptrdiff_t offset)
    {
        unsigned capacity = m_ptrToOffsetMap.capacity();
        m_ptrToOffsetMap.add(ptr, offset);
        chargeTableGrowth(capacity, m_ptrToOffsetMap.capacity(), sizeof(typename decltype(m_ptrToOffsetMap)::KeyValuePairType));
    }

    // Byte-identical immutable arrays (instruction streams, expression info, jump tables of small functions repeat a lot)
    // are stored once; later occurrences point at the first. Decoded objects are per code block either way.
    std::optional<ptrdiff_t> existingIdenticalArray(std::span<const uint8_t> bytes, unsigned hash, size_t alignment)
    {
        auto it = m_arraysByHash.find(hash);
        if (it == m_arraysByHash.end())
            return std::nullopt;
        for (auto [candidate, size] : it->value) {
            // An earlier copy made for a less-aligned element type may sit at an offset this one cannot use.
            if (size == bytes.size() && !(candidate % alignment) && equalSpans(bytesAt(candidate, size), bytes))
                return candidate;
        }
        return std::nullopt;
    }
    void registerArray(unsigned hash, ptrdiff_t offset, size_t size)
    {
        unsigned capacity = m_arraysByHash.capacity();
        auto& candidates = m_arraysByHash.add(hash, Vector<std::pair<ptrdiff_t, size_t>, 1> { }).iterator->value;
        chargeTableGrowth(capacity, m_arraysByHash.capacity(), sizeof(typename decltype(m_arraysByHash)::KeyValuePairType));
        // A hash shared by several arrays spills its candidates out of their inline slot.
        size_t spilledBefore = encoderHeapBytes(candidates);
        candidates.append({ offset, size });
        chargeTableGrowth(spilledBefore, encoderHeapBytes(candidates), 1);
    }

    // Non-symbol strings decode to AtomStringImpl::add(characters), so two records with the same characters decode to the
    // same atom: write the characters once and point every user at them.
    std::optional<ptrdiff_t> cachedOffsetForStringContents(const StringImpl& string)
    {
        if (string.isSymbol() || !string.length())
            return std::nullopt;
        auto it = m_stringsByContents.find(String(const_cast<StringImpl*>(&string)));
        if (it == m_stringsByContents.end())
            return std::nullopt;
        return it->value;
    }
    void cacheStringContents(const StringImpl& string, ptrdiff_t offset)
    {
        if (string.isSymbol() || !string.length())
            return;
        unsigned capacity = m_stringsByContents.capacity();
        m_stringsByContents.add(String(const_cast<StringImpl*>(&string)), offset);
        chargeTableGrowth(capacity, m_stringsByContents.capacity(), sizeof(typename decltype(m_stringsByContents)::KeyValuePairType));
    }

    std::optional<ptrdiff_t> cachedOffsetForPtr(const void* ptr)
    {
        auto it = m_ptrToOffsetMap.find(ptr);
        if (it == m_ptrToOffsetMap.end())
            return std::nullopt;
        return { it->value };
    }

    void addLeafExecutable(const UnlinkedFunctionExecutable* executable, ptrdiff_t offset)
    {
        m_leafExecutables.add(executable, offset);
    }

    // Layout: a code block's own arrays and its children's executable records are written contiguously; the children's
    // bodies follow breadth-first, and data that is only read on rare paths (expression info) goes after every body.
    // Decoding one block then reads one contiguous run of the payload rather than records scattered through every
    // descendant's subtree, so a mapped payload pages in only what is decoded.
    template<typename Callable> void deferBody(Callable&& encodeBody) { defer(m_bodies, m_bodiesCapacity, std::forward<Callable>(encodeBody)); }
    template<typename Callable> void deferCold(Callable&& encodeCold) { defer(m_cold, m_coldCapacity, std::forward<Callable>(encodeCold)); }
    void encodeDeferred()
    {
        while (!m_bodies.isEmpty())
            m_bodies.takeFirst()();
        while (!m_cold.isEmpty()) {
            m_cold.takeFirst()();
            RELEASE_ASSERT(m_bodies.isEmpty());
        }
        // Slots inside a checksummed region (a block's ExpressionInfo, its children's records) are filled by the deferred
        // work above, so the checksums are computed only now that every byte is final.
        for (auto& pending : m_pendingChecksums) {
            uint32_t crc = ~checksumOfRange(pending.start, pending.size, pending.checksumOffset);
            for (auto [offset, size] : pending.externalArrays)
                crc = crc32c(crc, bytesAt(offset, size));
            uint32_t checksum = ~crc;
            memcpySpan(mutableBytesAt(pending.checksumOffset, sizeof(checksum)), std::span { reinterpret_cast<const uint8_t*>(&checksum), sizeof(checksum) });
        }
        m_pendingChecksums.clear();
    }
    void addChecksum(ptrdiff_t start, size_t size, ptrdiff_t checksumOffset, Vector<std::pair<ptrdiff_t, size_t>>&& externalArrays = { }) { m_pendingChecksums.append({ start, size, checksumOffset, WTF::move(externalArrays) }); }
    uint32_t nextStringOrdinal() { return m_numberStrings ? m_nextStringOrdinal++ : std::numeric_limits<uint32_t>::max(); }

    // Content-sharing of arrays is only on while a code block encodes the few arrays its checksum knows how to follow
    // (decoder side: CachedCodeBlock::regionIsIntact); an array shared from outside the block's own bytes is folded into
    // the block's checksum so it is verified by whoever reads it, not only by whoever wrote it first.
    class ShareableArrayScope {
    public:
        ShareableArrayScope(Encoder& encoder)
            : m_encoder(encoder)
            , m_previous(std::exchange(encoder.m_arraySharingEnabled, true))
        {
        }
        ~ShareableArrayScope() { m_encoder.m_arraySharingEnabled = m_previous; }

    private:
        Encoder& m_encoder;
        bool m_previous;
    };
    bool arraySharingEnabled() const { return m_arraySharingEnabled; }
    void beginBlockRegion(ptrdiff_t start) { m_blockRegionStart = start; m_blockExternalArrays.clear(); }
    void noteSharedArray(ptrdiff_t offset, size_t size)
    {
        if (offset < m_blockRegionStart)
            m_blockExternalArrays.append({ offset, size });
    }
    Vector<std::pair<ptrdiff_t, size_t>> takeBlockExternalArrays() { return std::exchange(m_blockExternalArrays, { }); }

    RefPtr<CachedBytecode> release(BytecodeCacheError& error)
    {
        if (!m_currentPage)
            return nullptr;
        m_currentPage->alignEnd();

        if (m_fileHandle) {
            return releaseMapped(error);
        }

        size_t size = m_baseOffset + m_currentPage->size();
        auto buffer = MallocSpan<uint8_t, VMMalloc>::malloc(size);
        auto bufferSpan = buffer.mutableSpan();
        for (const auto& page : m_pages)
            memcpySpan(consumeSpan(bufferSpan, page.size()), page.span());
        RELEASE_ASSERT(bufferSpan.empty());
        return CachedBytecode::create(WTF::move(buffer), WTF::move(m_leafExecutables));
    }

    // The size release() would return, with the current page's end aligned as release() aligns it.
    size_t releasedSize()
    {
        m_currentPage->alignEnd();
        return m_baseOffset + m_currentPage->size();
    }

    // JITCache: each page's used span in payload order, the current page's end aligned as release() aligns it, so that the
    // spans concatenate to the payload release() would assemble (SPEC-ucb.codec.md, E13).
    void forEachPage(const ScopedLambda<void(std::span<const uint8_t>)>& functor)
    {
        m_currentPage->alignEnd();
        for (const auto& page : m_pages)
            functor(page.span());
    }

private:
    // Deque::expandCapacity takes a deque from no buffer to 16 entries and then doubles it whenever an append finds it
    // full, which a ring buffer is at one entry short of its capacity; the encoder follows it by counting entries (E8).
    template<typename Callable>
    void defer(Deque<Function<void()>>& deferred, size_t& capacity, Callable&& callable)
    {
        if (!capacity || deferred.size() + 1 == capacity) {
            size_t newCapacity = std::max<size_t>(16, capacity * 2);
            chargeTableGrowth(capacity, newCapacity, sizeof(Function<void()>));
            capacity = newCapacity;
        }
        chargeGrowth(sizeof(WTF::Detail::CallableWrapper<std::decay_t<Callable>, void>));
        deferred.append(Function<void()>(std::forward<Callable>(callable)));
    }

    RefPtr<CachedBytecode> releaseMapped(BytecodeCacheError& error)
    {
        size_t size = m_baseOffset + m_currentPage->size();
        if (!m_fileHandle.truncate(size)) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }

        for (const auto& page : m_pages) {
            auto bytesWritten = m_fileHandle.write(page.span());
            if (!bytesWritten) {
                error = BytecodeCacheError::StandardError(errno);
                return nullptr;
            }

            if (*bytesWritten != page.size()) {
                error = BytecodeCacheError::WriteError(*bytesWritten, page.size());
                return nullptr;
            }
        }

        auto mappedFileData = m_fileHandle.map(FileSystem::MappedFileMode::Private);
        if (!mappedFileData) {
            error = BytecodeCacheError::StandardError(errno);
            return nullptr;
        }

        return CachedBytecode::create(WTF::move(*mappedFileData), WTF::move(m_leafExecutables));
    }

    class Page {
    public:
        Page(size_t size)
            : m_buffer(MallocSpan<uint8_t, VMMalloc>::zeroedMalloc(size)) // alignment gaps end up in the file: keep them deterministic
        {
        }

        bool malloc(size_t size, size_t alignment, ptrdiff_t& result)
        {
            ASSERT(alignment && alignment <= encoderMaxAlignment && isPowerOfTwo(alignment));
            ptrdiff_t offset = roundUpToMultipleOf(alignment, m_offset);
            if (static_cast<size_t>(offset + size) > capacity())
                return false;

            result = offset;
            m_offset = offset + size;
            return true;
        }

        // FIXME: Port call sites for span() / mutableSpan() and remove.
        const uint8_t* NODELETE buffer() const { return m_buffer.span().data(); }
        uint8_t* NODELETE buffer() { return m_buffer.mutableSpan().data(); }
        size_t size() const { return static_cast<size_t>(m_offset); }

        std::span<uint8_t> mutableSpan() LIFETIME_BOUND { return m_buffer.mutableSpan().first(size()); }
        std::span<const uint8_t> span() const LIFETIME_BOUND { return m_buffer.span().first(size()); }

        bool NODELETE getOffset(const void* address, ptrdiff_t& result) const
        {
            auto* addr = static_cast<const uint8_t*>(address);
            auto* bufferStart = buffer();
            if (addr >= bufferStart && addr < bufferStart + m_offset) {
                result = addr - bufferStart;
                return true;
            }
            return false;
        }

        void NODELETE alignEnd()
        {
            ptrdiff_t size = roundUpToMultipleOf(encoderMaxAlignment, m_offset);
            if (size == m_offset)
                return;
            RELEASE_ASSERT(static_cast<size_t>(size) <= capacity());
            m_offset = size;
        }

    private:
        size_t capacity() const { return m_buffer.sizeInBytes(); }

        MallocSpan<uint8_t, VMMalloc> m_buffer;
        ptrdiff_t m_offset { 0 };
    };

    void allocateNewPage(size_t size = 0)
    {
        static constexpr size_t minPageSize = encoderMinPageSize;
        if (m_currentPage) {
            m_currentPage->alignEnd();
            m_baseOffset += m_currentPage->size();
        }
        // Grow geometrically so offsetOf()/bytesAt(), which walk the page list, stay cheap on large payloads. A JITCache core
        // starts at one minimum page, since most cores are small and every encode zeroes its first page; the policy is part
        // of the payload format, because a page boundary moves padding (SPEC-ucb.codec.md, E12).
        size_t pageShift = m_pages.size() + (isCore() ? 0 : 4);
        size_t preferred = minPageSize << std::min<size_t>(pageShift, 14);
        if (size < preferred)
            size = preferred;
        else
            size = roundUpToMultipleOf(minPageSize, size);
        chargeGrowth(size);
        size_t pagesCapacity = m_pages.capacity();
        m_pages.append(Page { size });
        chargeTableGrowth(pagesCapacity, m_pages.capacity(), sizeof(Page));
        m_currentPage = &m_pages.last();
    }

    VM& m_vm;
    FileSystem::FileHandle& m_fileHandle;
    ptrdiff_t m_baseOffset;
    Page* m_currentPage;
    Vector<Page> m_pages;
    UncheckedKeyHashMap<const void*, ptrdiff_t> m_ptrToOffsetMap;
    HashMap<String, ptrdiff_t> m_stringsByContents; // keyed by contents (StringHash), not identity
    LeafExecutableMap m_leafExecutables;
    Deque<Function<void()>> m_bodies;
    Deque<Function<void()>> m_cold;
    struct PendingChecksum { ptrdiff_t start; size_t size; ptrdiff_t checksumOffset; Vector<std::pair<ptrdiff_t, size_t>> externalArrays; };
    Vector<PendingChecksum> m_pendingChecksums;
    uint32_t m_nextStringOrdinal { 0 };
    EncoderStringTable* m_externalStrings;
    bool m_numberStrings;
    bool m_updatable;
    bool m_checksums;
    const Purpose m_purpose;
    CoreEncodingBudget* const m_budget;
    const TDZEnvironmentLink* const m_startChain;
    bool m_budgetRefused { false };
    size_t m_chargedBytes { 0 };
    size_t m_bodiesCapacity { 0 };
    size_t m_coldCapacity { 0 };
    Vector<SharedPrivateNameEnvironment> m_sharedPrivateNameEnvironments;
    bool m_arraySharingEnabled { false };
    ptrdiff_t m_blockRegionStart { 0 };
    Vector<std::pair<ptrdiff_t, size_t>> m_blockExternalArrays;
    UncheckedKeyHashMap<unsigned, Vector<std::pair<ptrdiff_t, size_t>, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_arraysByHash;
};

// JITCache: the scratch an encode builds and frees within one record (SPEC-ucb.codec.md, E8), charged from when it is
// allocated until the scope that frees it ends.
class EncoderScratchCharge {
    WTF_MAKE_NONCOPYABLE(EncoderScratchCharge);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    EncoderScratchCharge(Encoder& encoder, size_t bytes)
        : m_encoder(encoder)
        , m_bytes(bytes)
    {
        encoder.chargeGrowth(bytes);
    }
    ~EncoderScratchCharge() { m_encoder.releaseGrowth(m_bytes); }

private:
    Encoder& m_encoder;
    size_t m_bytes;
};

Decoder::Decoder(VM& vm, Ref<CachedBytecode> cachedBytecode, RefPtr<SourceProvider> provider, Purpose purpose, RefPtr<TDZEnvironmentLink> startChain, bool validate)
    : m_vm(vm)
    , m_cachedBytecode(WTF::move(cachedBytecode))
    , m_provider(provider)
    , m_purpose(purpose)
    , m_startChain(WTF::move(startChain))
    , m_validates(validate)
{
    // Only a JITCache core is decoded fallibly, and only a fallible decode validates (SPEC-ucb.codec.md, section 4).
    ASSERT(!m_validates || m_purpose == Purpose::JITCacheCore);
    ASSERT(!m_startChain || m_purpose == Purpose::JITCacheCore);
}

Decoder::~Decoder()
{
    for (AtomStringImpl* atom : m_atomsByOrdinal) {
        if (atom)
            atom->deref();
    }
    for (auto& finalizer : m_finalizers)
        finalizer();
}

Ref<Decoder> Decoder::create(VM& vm, Ref<CachedBytecode> cachedBytecode, RefPtr<SourceProvider> provider, Purpose purpose, RefPtr<TDZEnvironmentLink> startChain, bool validate)
{
    return adoptRef(*new Decoder(vm, WTF::move(cachedBytecode), WTF::move(provider), purpose, WTF::move(startChain), validate));
}

Decoder::Purpose Decoder::purpose() const
{
    return m_purpose;
}

bool Decoder::isFallible() const
{
    return m_purpose == Purpose::JITCacheCore;
}

bool Decoder::validates() const
{
    return m_validates;
}

void Decoder::noteFailure(CoreDecodeFailure failure)
{
    ASSERT(isFallible() && failure != CoreDecodeFailure::None);
    if (m_failure == CoreDecodeFailure::None)
        m_failure = failure;
}

CoreDecodeFailure Decoder::failure() const
{
    return m_failure;
}

TDZEnvironmentLink* Decoder::startChain() const
{
    return m_startChain.get();
}

size_t Decoder::size() const
{
    return m_cachedBytecode->size();
}

ptrdiff_t Decoder::offsetOf(const void* ptr)
{
    auto* addr = static_cast<const uint8_t*>(ptr);
    auto cachedBytecodeSpan = m_cachedBytecode->span();
    ASSERT(addr >= cachedBytecodeSpan.data() && addr < std::to_address(cachedBytecodeSpan.end()));
    return addr - cachedBytecodeSpan.data();
}

void Decoder::cacheOffset(ptrdiff_t offset, void* ptr)
{
    m_offsetToPtrMap.add(offset, ptr);
}

std::optional<void*> Decoder::cachedPtrForOffset(ptrdiff_t offset)
{
    auto it = m_offsetToPtrMap.find(offset);
    if (it == m_offsetToPtrMap.end())
        return std::nullopt;
    return { it->value };
}

const void* Decoder::ptrForOffsetFromBase(ptrdiff_t offset)
{
    ASSERT(offset > 0 && static_cast<size_t>(offset) < m_cachedBytecode->size());
    return m_cachedBytecode->span().subspan(offset).data();
}

CompactTDZEnvironmentMap::Handle Decoder::handleForTDZEnvironment(CompactTDZEnvironment* environment) const
{
    auto it = m_environmentToHandleMap.find(environment);
    RELEASE_ASSERT(it != m_environmentToHandleMap.end());
    return it->value;
}

void Decoder::setHandleForTDZEnvironment(CompactTDZEnvironment* environment, const CompactTDZEnvironmentMap::Handle& handle)
{
    auto addResult = m_environmentToHandleMap.add(environment, handle);
    RELEASE_ASSERT(addResult.isNewEntry);
}

void Decoder::addLeafExecutable(const UnlinkedFunctionExecutable* executable, ptrdiff_t offset)
{
    // JITCache: a core's CachedBytecode dies with its decode, so no update ever appends to it (SPEC-ucb.codec.md, E7).
    if (m_purpose == Purpose::JITCacheCore)
        return;
#if USE(BUN_JSC_ADDITIONS)
    // Only CachedBytecode::addFunctionUpdate reads this map, and Bun never calls it.
    if (Options::useLeanBytecodeCacheDecoder())
        return;
#endif
    m_cachedBytecode->leafExecutables().add(executable, offset);
}

template<typename Functor>
void Decoder::addFinalizer(const Functor& fn)
{
    m_finalizers.append(fn);
}

RefPtr<SourceProvider> Decoder::provider() const
{
    return m_provider;
}

// JITCache: the validating decode of a core (SPEC-ucb.codec.md, E15). A check that fails records Malformed, and the decode
// takes the record as the empty value of its type and goes on, so that it finishes without reading outside the payload,
// asserting or crashing; decodeUnlinkedCodeBlockCore then discards the UCB.

// Whether `size` bytes at `address` lie inside the payload, aligned to `alignment`. Integers, so that an offset read from a
// damaged payload makes no out-of-range pointer.
static bool coreCodecHolds(Decoder& decoder, uintptr_t address, size_t size, size_t alignment)
{
    auto payload = decoder.payloadSpan();
    uintptr_t begin = std::bit_cast<uintptr_t>(payload.data());
    return !(address % alignment) && address >= begin && size <= payload.size() && address - begin <= payload.size() - size;
}

static bool coreCodecHolds(Decoder& decoder, const void* start, size_t size, size_t alignment)
{
    return coreCodecHolds(decoder, std::bit_cast<uintptr_t>(start), size, alignment);
}

// Records Malformed when `condition` is false, and returns it.
static bool coreCodecCheck(Decoder& decoder, bool condition)
{
    if (!condition) [[unlikely]]
        decoder.noteFailure(CoreDecodeFailure::Malformed);
    return condition;
}

// A record type with a tail or tags of its own checks them in isWellFormed(Decoder&); the others are well formed once placed.
template<typename T>
static bool coreCodecIsWellFormed(Decoder& decoder, const T& record)
{
    if constexpr (requires { record.isWellFormed(decoder); })
        return record.isWellFormed(decoder);
    else
        return true;
}

// HashTable::add and InlineMap::add assert against these keys, and a string key's hash dereferences it.
template<typename KeyTraits, typename Key>
static bool coreCodecIsEmptyOrDeletedKey(const Key& key)
{
    return WTF::isHashTraitsEmptyValue<KeyTraits>(key) || KeyTraits::isDeletedValue(key);
}

// JITCache: the pieces of the digests of SPEC-ucb.md section 3.4 that this file computes. Integers are little-endian.
static void coreCodecHashU32(JITCache::SHA256& hasher, uint32_t value)
{
    std::array<uint8_t, 4> bytes { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24) };
    hasher.update(bytes);
}

// A canonical string (SPEC-ucb.md section 3.3): u8 encoding, u32 length, then the code units, one byte each (encoding 1)
// exactly when every unit is at most 0xFF and UTF-16LE (encoding 2) otherwise; a null string is encoding 0 with length 0.
static void coreCodecHashCanonicalString(JITCache::SHA256& hasher, const StringImpl* string)
{
    static_assert(std::endian::native == std::endian::little, "UTF-16 units are hashed as they lie in memory");
    if (!string) {
        uint8_t encoding = 0;
        hasher.update(std::span { &encoding, 1 });
        coreCodecHashU32(hasher, 0);
        return;
    }
    bool isLatin1 = string->is8Bit() || WTF::charactersAreAllLatin1(string->span16());
    uint8_t encoding = isLatin1 ? 1 : 2;
    hasher.update(std::span { &encoding, 1 });
    coreCodecHashU32(hasher, string->length());
    if (string->is8Bit()) {
        hasher.update(string->span8());
        return;
    }
    std::span<const char16_t> units = string->span16();
    if (!isLatin1) {
        hasher.update(asBytes(units));
        return;
    }
    std::array<uint8_t, 256> narrowed;
    while (!units.empty()) {
        size_t count = std::min(units.size(), narrowed.size());
        for (size_t i = 0; i < count; ++i)
            narrowed[i] = static_cast<uint8_t>(units[i]);
        hasher.update(std::span { narrowed }.first(count));
        units = units.subspan(count);
    }
}

template<typename T>
static void encode(Encoder& encoder, T& dst, const SourceType<T>& src)
{
    if constexpr (std::is_same_v<T, SourceType<T>>)
        dst = src;
    else
        dst.encode(encoder, src);
}

template<typename T, typename... Args>
static void decode(Decoder& decoder, const T& src, SourceType<T>& dst, Args... args)
{
    if constexpr (std::is_same_v<T, SourceType<T>>)
        dst = src;
    else
        src.decode(decoder, dst, args...);
}

template<typename Source>
class CachedObject {
    WTF_MAKE_NONCOPYABLE(CachedObject);

public:
    using SourceType_ = Source;

    CachedObject() = default;

    inline void* operator new(size_t, void* where) { return where; }
    void* operator new[](size_t, void* where) { return where; }

    // Copied from WTF_FORBID_HEAP_ALLOCATION, since we only want to allow placement new
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;
    void* operator new[](size_t size) = delete;
    void operator delete[](void*) = delete;
    void* operator new(size_t, NotNullTag, void* location) = delete;
};

template<typename Source>
class VariableLengthObject : public CachedObject<Source>, VariableLengthObjectBase {
    template<typename, typename>
    friend class CachedPtr;
    friend struct CachedPtrOffsets;
    friend struct CoreCodecSelfTestAccess;

public:
    using typename VariableLengthObjectBase::Offset;

    VariableLengthObject()
        : VariableLengthObjectBase(s_invalidOffset)
    {
    }

    bool NODELETE isEmpty() const
    {
        return m_offset == s_invalidOffset;
    }

    // Encoder side: where this object's payload landed, as a payload offset (encoder pages are not contiguous in memory,
    // so `this + m_offset` is only meaningful once decoded).
    ptrdiff_t payloadOffsetInEncoder(Encoder& encoder) const { return encoder.offsetOf(&this->m_offset) + this->m_offset; }
    // Encoder side: point at something already written instead of allocating.
    void pointAtPayloadOffset(Encoder& encoder, ptrdiff_t offset) { this->m_offset = safeCast<Offset>(offset - encoder.offsetOf(&this->m_offset)); }

    // A 1-3 character Latin-1 string that decodes to an atom fits in the 4-byte slot that would otherwise hold the offset
    // of its record: low two bits 01 (record offsets are multiples of 4 and the empty sentinel ends in 11), then the
    // length, then the characters. Minified code is mostly such names.
    bool tryEncodeInlineString(const StringImpl& string)
    {
        std::optional<uint32_t> packed = packInlineString(string);
        if (!packed)
            return false;
        m_offset = std::bit_cast<Offset>(*packed);
        return true;
    }
    bool NODELETE hasInlineString() const { return (static_cast<uint32_t>(m_offset) & inlineStringTagMask) == inlineStringTag; }
    // The encoder writes one to three characters inline, never none.
    bool NODELETE hasWellFormedInlineString() const { return (static_cast<uint32_t>(m_offset) >> 2) & 3; }
    // The slot as a plain value, for owners whose kind byte says it holds one rather than an offset.
    uint32_t NODELETE rawSlot() const { return std::bit_cast<uint32_t>(m_offset); }
    void setRawSlot(uint32_t value) { m_offset = std::bit_cast<Offset>(value); }
    Ref<AtomStringImpl> inlineString(Decoder& decoder) const { return decoder.atomForInlineString(asByteSpan<Offset, sizeof(Offset)>(m_offset)); }

    // A ≥4-char non-symbol string held in the embedder's shared EncoderStringTable/DecoderStringTable: the slot is an ordinal into that one process-wide table, so every chunk's payload carries 4 bytes instead of a full record. Tag 10 is the value low-two-bits neither a 4-aligned record offset (00), an inline string (01), nor the empty sentinel (11) can produce.
    bool NODELETE hasExternalString() const { return (static_cast<uint32_t>(m_offset) & inlineStringTagMask) == externalStringTag; }
    uint32_t NODELETE externalStringOrdinal() const { return static_cast<uint32_t>(std::bit_cast<uint32_t>(m_offset)) >> 2; }
    bool tryEncodeExternalString(Encoder& encoder, const StringImpl& string)
    {
        if (!encoder.externalStrings() || string.isSymbol() || !string.length())
            return false;
        uint32_t ordinal = encoder.externalStrings()->ordinalFor(string);
        if (ordinal > EncoderStringTable::maxOrdinal) [[unlikely]]
            return false;
        m_offset = std::bit_cast<Offset>(externalStringTag | ordinal << 2);
        return true;
    }

    // JITCache: what this slot points at when it is not empty and `count` elements of T there, a size computed with overflow
    // checks, lie wholly inside the payload and are aligned for T; null otherwise. The validating decode reaches every
    // record and array through it (SPEC-ucb.codec.md, E15).
    template<typename T>
    const T* placedTarget(Decoder& decoder, size_t count = 1) const
    {
        CheckedSize size = count;
        size *= sizeof(T);
        if (size.hasOverflowed())
            return nullptr;
        return std::bit_cast<const T*>(placedBytes(decoder, size.value(), alignof(T)));
    }
    const uint8_t* placedBytes(Decoder& decoder, size_t size, size_t alignment) const
    {
        if (isEmpty())
            return nullptr;
        uintptr_t address = std::bit_cast<uintptr_t>(this) + static_cast<intptr_t>(m_offset);
        if (!coreCodecHolds(decoder, address, size, alignment))
            return nullptr;
        return std::bit_cast<const uint8_t*>(address);
    }

protected:
    const uint8_t* NODELETE buffer() const
    {
        ASSERT(!isEmpty());
        return std::bit_cast<const uint8_t*>(this) + m_offset;
    }

    template<typename T>
    const T* NODELETE buffer() const
    {
        ASSERT(!(std::bit_cast<uintptr_t>(buffer()) % alignof(T)));
        return std::bit_cast<const T*>(buffer());
    }

    uint8_t* allocate(Encoder& encoder, size_t size, size_t alignment)
    {
        ptrdiff_t offsetOffset = encoder.offsetOf(&m_offset);
        auto result = encoder.malloc(size, alignment);
        m_offset = safeCast<Offset>(result.offset() - offsetOffset);
        return result.buffer();
    }

    template<typename T>
#if CPU(ARM64) && CPU(ADDRESS32)
    // FIXME: Remove this once it's no longer needed and LLVM doesn't miscompile us:
    // <rdar://problem/49792205>
    __attribute__((optnone))
#endif
    T* allocate(Encoder& encoder, unsigned size = 1)
    {
        static_assert(PayloadType<T>);
        uint8_t* result = allocate(encoder, sizeof(T) * size, alignof(T));
        ASSERT(!(std::bit_cast<uintptr_t>(result) % alignof(T)));
        return new (result) T[size];
    }

    // For arrays whose encoding is a plain copy of the source bytes: share an earlier identical array if there is one.
    void allocateOrShareBytes(Encoder& encoder, std::span<const uint8_t> bytes, size_t alignment)
    {
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(bytes) ^ static_cast<unsigned>(bytes.size());
        if (encoder.arraySharingEnabled()) {
            if (auto existing = encoder.existingIdenticalArray(bytes, hash, alignment)) {
                m_offset = safeCast<Offset>(*existing - encoder.offsetOf(&m_offset));
                encoder.noteSharedArray(*existing, bytes.size());
                return;
            }
        }
        ptrdiff_t offsetOffset = encoder.offsetOf(&m_offset);
        auto result = encoder.malloc(bytes.size(), alignment);
        m_offset = safeCast<Offset>(result.offset() - offsetOffset);
        memcpySpan(std::span { result.buffer(), bytes.size() }, bytes);
        encoder.registerArray(hash, result.offset(), bytes.size());
    }

    // One T followed, in the same allocation, by the variable-length tail T asks for (see VarintWriter).
    template<typename T, typename SourceArg>
    T* allocateFor(Encoder& encoder, const SourceArg& source)
    {
        size_t tail = 0;
        if constexpr (requires { T::tailSize(encoder, source); })
            tail = T::tailSize(encoder, source);
        else if constexpr (requires { T::tailSize(source); })
            tail = T::tailSize(source);
        static_assert(PayloadType<T>);
        uint8_t* result = allocate(encoder, sizeof(T) + tail, alignof(T));
        return new (result) T();
    }

private:
    constexpr static Offset s_invalidOffset = std::numeric_limits<Offset>::max();
};

template<typename T, typename Source = SourceType<T>>
class CachedArray : public VariableLengthObject<Source*> {
public:
    void encode(Encoder& encoder, const Source* array, unsigned size)
    {
        if (!size)
            return;
        if constexpr (std::is_same_v<T, Source> && std::is_trivially_copyable_v<T>) {
            this->allocateOrShareBytes(encoder, std::span { std::bit_cast<const uint8_t*>(array), sizeof(T) * size }, alignof(T));
            return;
        }
        T* dst = this->template allocate<T>(encoder, size);
        for (unsigned i = 0; i < size; ++i)
            ::JSC::encode(encoder, dst[i], array[i]);
    }

    template<typename... Args>
    void decode(Decoder& decoder, Source* array, unsigned size, Args... args) const
    {
        if (!size)
            return;
        const T* buffer = decoder.validates() ? this->template placedTarget<T>(decoder, size) : this->template buffer<T>();
        if (!coreCodecCheck(decoder, !!buffer))
            return;
        for (unsigned i = 0; i < size; ++i)
            ::JSC::decode(decoder, buffer[i], array[i], args...);
    }

    // Raw view of the encoded elements, for element types whose encoding is the identity.
    const T* borrow() const
    {
        static_assert(std::is_same_v<T, Source> && std::is_trivially_copyable_v<T>);
        return this->isEmpty() ? nullptr : this->template buffer<T>();
    }
    const void* rawElements() const { return this->isEmpty() ? nullptr : this->buffer(); } // decoded side only
};

#if USE(BUN_JSC_ADDITIONS)
// A cached type declares `static constexpr bool isSingleOwner = true` when the Encoder
// only ever reaches it through one CachedPtr, so there is nothing for the
// ptr <-> offset maps to deduplicate on either side.
template<typename T> inline constexpr bool isSingleOwnerCachedType = requires { T::isSingleOwner; };

// A cached type declares `static constexpr bool decodesToCanonicalObject = true` when its
// decode() returns a +1 reference to an object that is already unique for its content
// (atoms, registry symbols), so shared references can be re-decoded instead of mapped.
template<typename T> inline constexpr bool isCanonicalCachedType = requires { T::decodesToCanonicalObject; };
#endif

class CachedUniquedStringImpl;
class CachedStringImpl;
class CachedTDZEnvironmentLink;

// JITCache: the validating decode's record cache, kept in the decoder's offset cache (SPEC-ucb.codec.md, E15). For each
// record a slot reaches, it holds the type the record is decoded as, from the moment its decode starts, and the object the
// decode gave, once it returns. The two keys, 2 * offset + 1 and 2 * offset + 2, are never 0 or -1, the empty and deleted
// keys of the cache's table. Only a validating decode reaches these helpers, and it never takes the native path that
// caches by plain offset.
enum class CoreCodecRecordState : uint8_t {
    Unvisited,
    InProgress, // reached again from inside its own decode
    Decoded,
    OtherType,
};

// The record type a slot decodes. The two string slot types read one record, which the encoder shares between them by
// content and which decodes alike through either.
template<typename T> struct CoreCodecRecordType {
    using type = T;
};
template<> struct CoreCodecRecordType<CachedStringImpl> {
    using type = CachedUniquedStringImpl;
};

// One address per record type, in writable data, which no linker folds.
template<typename T> struct CoreCodecRecordTag {
    static inline char tag { 0 };
};

template<typename T>
static void* coreCodecRecordTag()
{
    return &CoreCodecRecordTag<typename CoreCodecRecordType<T>::type>::tag;
}

static ptrdiff_t coreCodecObjectKey(ptrdiff_t offset) { return 2 * offset + 1; }
static ptrdiff_t coreCodecTypeKey(ptrdiff_t offset) { return 2 * offset + 2; }

// `object` is set only for Decoded, to what the record's decode returned, null when it failed.
template<typename T>
static CoreCodecRecordState coreCodecRecordState(Decoder& decoder, const void* record, void*& object)
{
    ptrdiff_t offset = decoder.offsetOf(record);
    std::optional<void*> type = decoder.cachedPtrForOffset(coreCodecTypeKey(offset));
    if (!type)
        return CoreCodecRecordState::Unvisited;
    if (*type != coreCodecRecordTag<T>())
        return CoreCodecRecordState::OtherType;
    std::optional<void*> decoded = decoder.cachedPtrForOffset(coreCodecObjectKey(offset));
    if (!decoded)
        return CoreCodecRecordState::InProgress;
    object = *decoded;
    return CoreCodecRecordState::Decoded;
}

// Before the record's decode starts.
template<typename T>
static void coreCodecMarkRecord(Decoder& decoder, const void* record)
{
    decoder.cacheOffset(coreCodecTypeKey(decoder.offsetOf(record)), coreCodecRecordTag<T>());
}

// Once the record's decode returns.
static void coreCodecSetRecordObject(Decoder& decoder, const void* record, void* object)
{
    decoder.cacheOffset(coreCodecObjectKey(decoder.offsetOf(record)), object);
}

// A record the encoder writes once per owner (isSingleOwner), whose decoded object has that one owner.
template<typename T> inline constexpr bool coreCodecHasSingleOwner = requires { T::isSingleOwner; };

template<typename T, typename Source = SourceType<T>>
class CachedPtr : public VariableLengthObject<Source*> {
    template<typename, typename, typename>
    friend class CachedRefPtr;

    friend struct CachedPtrOffsets;

public:
    static constexpr bool holdsString = std::is_same_v<T, CachedUniquedStringImpl> || std::is_same_v<T, CachedStringImpl>;

    void encode(Encoder& encoder, const Source* src)
    {
        if (!src)
            return;
        if constexpr (holdsString) {
            if (!encoder.externalStrings() && this->tryEncodeInlineString(*src))
                return;
            if (this->tryEncodeExternalString(encoder, *src))
                return;
        }

        if constexpr (requires (Encoder& e, const Source& s) { T::create(e, s); }) {
            // Code blocks write their arrays first and their record after, so they place themselves.
            T* record = T::create(encoder, *src);
            this->m_offset = safeCast<VariableLengthObjectBase::Offset>(encoder.offsetOf(record) - encoder.offsetOf(&this->m_offset));
            return;
        } else
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isSingleOwnerCachedType<T>) {
            ASSERT(!encoder.cachedOffsetForPtr(src));
            this->template allocateFor<T>(encoder, *src)->encode(encoder, *src);
            return;
        } else
#endif
        {

        if (std::optional<ptrdiff_t> offset = encoder.cachedOffsetForPtr(src)) {
            this->m_offset = safeCast<VariableLengthObjectBase::Offset>(*offset - encoder.offsetOf(&this->m_offset));
            return;
        }
        if constexpr (holdsString) {
            if (std::optional<ptrdiff_t> offset = encoder.cachedOffsetForStringContents(*src)) {
                this->m_offset = safeCast<VariableLengthObjectBase::Offset>(*offset - encoder.offsetOf(&this->m_offset));
                encoder.cachePtr(src, *offset);
                return;
            }
        }

        T* cachedObject = this->template allocateFor<T>(encoder, *src);
        cachedObject->encode(encoder, *src);
        encoder.cachePtr(src, encoder.offsetOf(cachedObject));
        if constexpr (holdsString)
            encoder.cacheStringContents(*src, encoder.offsetOf(cachedObject));
        }
    }

    template<typename... Args>
    Source* decode(Decoder& decoder, bool& isNewAllocation, Args&&... args) const
    {
        if (this->isEmpty()) {
            isNewAllocation = false;
            return nullptr;
        }
        if constexpr (holdsString) {
            if (this->hasInlineString()) {
                if (decoder.validates() && !coreCodecCheck(decoder, this->hasWellFormedInlineString())) {
                    isNewAllocation = false;
                    return nullptr;
                }
                isNewAllocation = true;
                return static_cast<Source*>(&this->inlineString(decoder).leakRef());
            }
            if (this->hasExternalString()) {
                // A JITCacheCore encoder never writes an external-string ordinal (SPEC-ucb.codec.md, E15).
                if (decoder.validates() && !coreCodecCheck(decoder, false)) {
                    isNewAllocation = false;
                    return nullptr;
                }
                isNewAllocation = true;
                return static_cast<Source*>(&decoder.atomForExternalString(this->externalStringOrdinal()).leakRef());
            }
        }

        if (decoder.validates())
            return decodeValidated(decoder, isNewAllocation, std::forward<Args>(args)...);

#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isSingleOwnerCachedType<T>) {
            if (Options::useLeanBytecodeCacheDecoder()) {
                isNewAllocation = true;
                return get()->decode(decoder, std::forward<Args>(args)...);
            }
        }
#endif

        ptrdiff_t bufferOffset = decoder.offsetOf(this->buffer());
        if (std::optional<void*> ptr = decoder.cachedPtrForOffset(bufferOffset)) {
            isNewAllocation = false;
            return static_cast<Source*>(*ptr);
        }

        isNewAllocation = true;
        Source* ptr = get()->decode(decoder, std::forward<Args>(args)...);
        decoder.cacheOffset(bufferOffset, ptr);
        return ptr;
    }

    template<typename... Args>
    Source* decode(Decoder& decoder, Args&&... args) const
    {
        bool unusedIsNewAllocation;
        return decode(decoder, unusedIsNewAllocation, std::forward<Args>(args)...);
    }

    // JITCache: the validating decode (SPEC-ucb.codec.md, E15). The record must lie inside the payload, aligned, and be well
    // formed for its type, and it is checked and decoded once, through the record cache: a later slot naming it as the
    // same type shares the object its decode gave, as the native decode shares, and fails when another type named it first,
    // when its decode has not returned yet, which is a loop, or when the encoder writes the record once per owner, since
    // sharing its object would give that object two owners. A failed record decodes as null, and isNewAllocation is set only
    // for the non-null result of this slot's own decode.
    template<typename... Args>
    Source* decodeValidated(Decoder& decoder, bool& isNewAllocation, Args&&... args) const
    {
        isNewAllocation = false;
        const T* record = this->template placedTarget<T>(decoder);
        if (!coreCodecCheck(decoder, !!record))
            return nullptr;
        void* decoded = nullptr;
        switch (coreCodecRecordState<T>(decoder, record, decoded)) {
        case CoreCodecRecordState::Unvisited:
            break;
        case CoreCodecRecordState::Decoded:
            if (!coreCodecCheck(decoder, !coreCodecHasSingleOwner<T>))
                return nullptr;
            return static_cast<Source*>(decoded);
        case CoreCodecRecordState::InProgress:
        case CoreCodecRecordState::OtherType:
            decoder.noteFailure(CoreDecodeFailure::Malformed);
            return nullptr;
        }
        coreCodecMarkRecord<T>(decoder, record);
        Source* result = nullptr;
        if (coreCodecCheck(decoder, coreCodecIsWellFormed(decoder, *record)))
            result = record->decode(decoder, std::forward<Args>(args)...);
        coreCodecSetRecordObject(decoder, record, result);
        isNewAllocation = !!result;
        return result;
    }

    const T* NODELETE operator->() const { return get(); }

    // For integrity checks before anything is decoded: the target if it lies inside the payload, else null.
    const T* getIfInPayload(Decoder& decoder) const
    {
        if (this->isEmpty())
            return nullptr;
        if constexpr (holdsString) {
            if (this->hasInlineString() || this->hasExternalString())
                return nullptr;
        }
        const T* target = this->template buffer<T>();
        return decoder.payloadContains(target, sizeof(T)) ? target : nullptr;
    }

private:
    const T* NODELETE get() const
    {
        RELEASE_ASSERT(!this->isEmpty());
        return this->template buffer<T>();
    }
};

ptrdiff_t CachedPtrOffsets::offsetOffset()
{
    return OBJECT_OFFSETOF(CachedPtr<void>, m_offset);
}

template<typename T, typename Source = SourceType<T>, typename PtrTraits = RawPtrTraits<Source>>
class CachedRefPtr : public CachedObject<RefPtr<Source, PtrTraits>> {
public:
    void encode(Encoder& encoder, const Source* src)
    {
        m_ptr.encode(encoder, src);
    }

    void encode(Encoder& encoder, const RefPtr<Source, PtrTraits> src)
    {
        encode(encoder, src.get());
    }

    const CachedPtr<T, Source>& ptr() const { return m_ptr; }
    CachedPtr<T, Source>& ptr() { return m_ptr; }

    RefPtr<Source, PtrTraits> decode(Decoder& decoder) const
    {
#if USE(BUN_JSC_ADDITIONS)
        if constexpr (isCanonicalCachedType<T>) {
            // The validating decode checks the record CachedPtr::decode reaches (SPEC-ucb.codec.md, E15).
            if (Options::useLeanBytecodeCacheDecoder() && !decoder.validates()) {
                if (m_ptr.isEmpty())
                    return nullptr;
                if constexpr (CachedPtr<T, Source>::holdsString) {
                    if (m_ptr.hasInlineString())
                        return adoptRef<Source, PtrTraits>(static_cast<Source*>(&m_ptr.inlineString(decoder).leakRef()));
                    if (m_ptr.hasExternalString())
                        return adoptRef<Source, PtrTraits>(static_cast<Source*>(&decoder.atomForExternalString(m_ptr.externalStringOrdinal()).leakRef()));
                }
                return adoptRef<Source, PtrTraits>(m_ptr.get()->decode(decoder));
            }
        }
#endif
        bool isNewAllocation;
        Source* decodedPtr = m_ptr.decode(decoder, isNewAllocation);
        if (!decodedPtr)
            return nullptr;
        if (isNewAllocation) {
            decoder.addFinalizer([=] {
                WTF::DefaultRefDerefTraits<Source>::derefIfNotNull(decodedPtr);
            });
        }
        auto result = adoptRef<Source, PtrTraits>(decodedPtr);
        result->ref();
        return result;
    }

    void decode(Decoder& decoder, RefPtr<Source, PtrTraits>& src) const
    {
        src = decode(decoder);
    }

private:
    CachedPtr<T, Source> m_ptr;
};

template<typename T, typename Source = SourceType<T>>
class CachedWriteBarrier : public CachedObject<WriteBarrier<Source>> {
    friend struct CachedWriteBarrierOffsets;

public:
    bool NODELETE isEmpty() const { return m_ptr.isEmpty(); }
    const CachedPtr<T, Source>& ptr() const { return m_ptr; }

    void encode(Encoder& encoder, const WriteBarrier<Source> src)
    {
        m_ptr.encode(encoder, src.get());
    }

    void decode(Decoder& decoder, WriteBarrier<Source>& src, const JSCell* owner) const
    {
        Source* decodedPtr = m_ptr.decode(decoder);
        if (decodedPtr)
            src.set(decoder.vm(), owner, decodedPtr);
    }

private:
    CachedPtr<T, Source> m_ptr;
};

ptrdiff_t CachedWriteBarrierOffsets::ptrOffset()
{
    return OBJECT_OFFSETOF(CachedWriteBarrier<void>, m_ptr);
}

template<typename T, size_t InlineCapacity = 0, typename OverflowHandler = CrashOnOverflow, typename Malloc = WTF::VectorBufferMalloc>
class CachedVector : public VariableLengthObject<Vector<SourceType<T>, InlineCapacity, OverflowHandler, 16, Malloc>> {
public:
    template<typename VectorContainer>
    void encode(Encoder& encoder, const VectorContainer& vector)
    {
        m_size = vector.size();
        if (!m_size)
            return;
        if constexpr (std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>) {
            this->allocateOrShareBytes(encoder, std::span { std::bit_cast<const uint8_t*>(vector.span().data()), sizeof(T) * m_size }, alignof(T));
            return;
        }
        T* buffer = this->template allocate<T>(encoder, m_size);
        for (unsigned i = 0; i < m_size; ++i)
            ::JSC::encode(encoder, buffer[i], vector[i]);
    }

    template<typename Range>
    void encodeRange(Encoder& encoder, unsigned size, const Range& range)
    {
        m_size = size;
        if (!m_size)
            return;
        T* buffer = this->template allocate<T>(encoder, m_size);
        unsigned i = 0;
        for (const auto& element : range)
            buffer[i++].encode(encoder, element);
    }

    template<typename... Args, typename VectorContainer>
    void decode(Decoder& decoder, VectorContainer& vector, Args... args) const
    {
        if (!m_size)
            return;
        // A count past the payload decodes as an empty container (SPEC-ucb.codec.md, E15).
        const T* buffer = decoder.validates() ? this->template placedTarget<T>(decoder, m_size) : this->template buffer<T>();
        if (!coreCodecCheck(decoder, !!buffer))
            return;
        vector = VectorContainer(m_size);
        for (unsigned i = 0; i < m_size; ++i)
            ::JSC::decode(decoder, buffer[i], vector[i], args...);
    }

    // Raw view of the encoded elements, for element types whose encoding is the identity.
    std::span<const T> borrow() const
    {
        static_assert(std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>);
        if (!m_size)
            return { };
        return { this->template buffer<T>(), m_size };
    }

    // Allocate the element slots now and let the caller encode into them later (used to keep a code block's own bytes
    // ahead of its children's records).
    template<typename VectorContainer>
    std::span<T> allocateElements(Encoder& encoder, const VectorContainer& vector)
    {
        m_size = vector.size();
        if (!m_size)
            return { };
        return { this->template allocate<T>(encoder, m_size), m_size };
    }

    // Encoder side: point at element slots an identical vector wrote earlier.
    void shareElements(Encoder& encoder, ptrdiff_t elements, unsigned size)
    {
        m_size = size;
        if (m_size)
            this->pointAtPayloadOffset(encoder, elements);
    }
    ptrdiff_t elementsOffset(Encoder& encoder) const { return this->payloadOffsetInEncoder(encoder); }

    // Encoder side: the slots allocateElements() made.
    std::span<T> mutableElements(Encoder& encoder)
    {
        if (!m_size)
            return { };
        auto bytes = encoder.mutableBytesAt(this->payloadOffsetInEncoder(encoder), sizeof(T) * m_size);
        return { reinterpret_cast<T*>(bytes.data()), m_size };
    }

    // Where the encoded elements are (decoded side), whether or not they are inside the payload; empty if none.
    std::span<const uint8_t> rawBytes() const
    {
        if (!m_size)
            return { };
        return { this->buffer(), sizeof(T) * m_size };
    }

    // The encoded elements themselves, bounds-checked, for integrity checks before decoding.
    std::span<const T> elementsIfInPayload(Decoder& decoder) const
    {
        if (!m_size)
            return { };
        const T* elements = this->template buffer<T>();
        if (!decoder.payloadContains(elements, sizeof(T) * m_size))
            return { };
        return { elements, m_size };
    }
    unsigned size() const { return m_size; }

private:
    friend struct CoreCodecSelfTestAccess;

    unsigned m_size;
};

// A hash table's iteration order can depend on the process (a robin-hood table seeds its hash with its own address), so
// tables are encoded in key order: by contents, then by the kind of StringImpl the key decodes to (keys equal in both
// would decode to one StringImpl, so no table holds two).
struct EncodingOrder {
    static unsigned kind(const StringImpl* string)
    {
        if (!string->isSymbol())
            return 0;
        auto& symbol = *static_cast<const SymbolImpl*>(string);
        return 1 + symbol.isRegistered() * 2 + symbol.isPrivate();
    }
    static bool less(unsigned a, unsigned b) { return a < b; }
    static bool less(const StringImpl* a, const StringImpl* b)
    {
        if (auto order = codePointCompare(StringView(*a), StringView(*b)); order != 0)
            return order < 0;
        return kind(a) < kind(b);
    }
    template<typename T, typename Traits> static bool less(const RefPtr<T, Traits>& a, const RefPtr<T, Traits>& b) { return less(a.get(), b.get()); }

    template<typename Entries, typename KeyOf>
    static void sort(Entries& entries, const KeyOf& keyOf)
    {
        std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) { return less(keyOf(a), keyOf(b)); });
    }
};

template<typename First, typename Second>
class CachedPair : public CachedObject<std::pair<SourceType<First>, SourceType<Second>>> {
public:
    void encode(Encoder& encoder, const std::pair<SourceType<First>, SourceType<Second>>& pair)
    {
        ::JSC::encode(encoder, m_first, pair.first);
        ::JSC::encode(encoder, m_second, pair.second);
    }

    template<typename Key, typename Value>
    void encode(Encoder& encoder, const WTF::KeyValuePair<Key, Value>& pair)
    {
        ::JSC::encode(encoder, m_first, pair.key);
        ::JSC::encode(encoder, m_second, pair.value);
    }

    void decode(Decoder& decoder, std::pair<SourceType<First>, SourceType<Second>>& pair) const
    {
        ::JSC::decode(decoder, m_first, pair.first);
        ::JSC::decode(decoder, m_second, pair.second);
    }

private:
    friend struct CoreCodecSelfTestAccess;

    First m_first;
    Second m_second;
};

template<typename Key, typename Value, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>, typename TableTraits = WTF::HashTableTraits>
class CachedHashMap : public CachedObject<HashMap<SourceType<Key>, SourceType<Value>, HashArg, KeyTraitsArg, MappedTraitsArg, TableTraits>> {
    template<typename K, typename V, WTF::ShouldValidateKey shouldValidateKey>
    using Map = HashMap<K, V, HashArg, KeyTraitsArg, MappedTraitsArg, TableTraits, shouldValidateKey>;

public:
    template<WTF::ShouldValidateKey shouldValidateKey>
    void encode(Encoder& encoder, const Map<SourceType<Key>, SourceType<Value>, shouldValidateKey>& map)
    {
        Vector<const typename std::remove_reference_t<decltype(map)>::KeyValuePairType*> entries;
        entries.reserveInitialCapacity(map.size());
        EncoderScratchCharge entriesCharge(encoder, encoderHeapBytes(entries));
        for (auto& entry : map)
            entries.append(&entry);
        EncodingOrder::sort(entries, [](auto* entry) -> const auto& { return entry->key; });
        m_entries.encodeRange(encoder, entries.size(), entries | std::views::transform([](auto* entry) -> const auto& { return *entry; }));
    }

    // A private-name environment: its entries are shared with an identical environment written earlier (decode rebuilds
    // the map from the entries, so their order does not matter).
    template<WTF::ShouldValidateKey shouldValidateKey>
    void encodeShared(Encoder& encoder, const Map<SourceType<Key>, SourceType<Value>, shouldValidateKey>& map)
    {
        Vector<std::pair<const UniquedStringImpl*, uint16_t>> entries;
        entries.reserveInitialCapacity(map.size());
        // Scratch unless the encoder keeps the entries as a new shared environment, which then owns the buffer and its charge.
        size_t entriesBytes = encoderHeapBytes(entries);
        encoder.chargeGrowth(entriesBytes);
        for (const auto& it : map)
            entries.append({ it.key.get(), it.value.bits() });
        std::sort(entries.begin(), entries.end());
        unsigned hash = computeHash(entries);
        if (auto existing = encoder.sharedPrivateNameEnvironment(hash, entries)) {
            m_entries.shareElements(encoder, *existing, map.size());
            encoder.releaseGrowth(entriesBytes);
            return;
        }
        encode(encoder, map);
        if (map.size())
            encoder.addSharedPrivateNameEnvironment(hash, WTF::move(entries), m_entries.elementsOffset(encoder));
        else
            encoder.releaseGrowth(entriesBytes);
    }

    template<WTF::ShouldValidateKey shouldValidateKey>
    void decode(Decoder& decoder, Map<SourceType<Key>, SourceType<Value>, shouldValidateKey>& map) const
    {
        SourceType<decltype(m_entries)> decodedEntries;
        m_entries.decode(decoder, decodedEntries);
        if (decodedEntries.isEmpty())
            return;
        map.reserveInitialCapacity(decodedEntries.size());
        for (auto& pair : decodedEntries) {
            // A malformed key decodes as the empty value, which the map cannot hold (SPEC-ucb.codec.md, E15).
            if (decoder.validates() && !coreCodecCheck(decoder, !coreCodecIsEmptyOrDeletedKey<KeyTraitsArg>(pair.first)))
                continue;
            map.add(WTF::move(pair.first), WTF::move(pair.second));
        }
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedVector<CachedPair<Key, Value>> m_entries;
};

template<typename Key, typename Value, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>>
using CachedMemoryCompactLookupOnlyRobinHoodHashMap = CachedHashMap<Key, Value, HashArg, KeyTraitsArg, MappedTraitsArg, WTF::MemoryCompactLookupOnlyRobinHoodHashTableTraits>;

template<typename Key, typename Value, unsigned Capacity, typename HashArg = DefaultHash<SourceType<Key>>, typename KeyTraitsArg = HashTraits<SourceType<Key>>, typename MappedTraitsArg = HashTraits<SourceType<Value>>>
class CachedInlineMap : public CachedObject<InlineMap<SourceType<Key>, SourceType<Value>, Capacity, HashArg, KeyTraitsArg, MappedTraitsArg>> {

    using Map = InlineMap<SourceType<Key>, SourceType<Value>, Capacity, HashArg, KeyTraitsArg, MappedTraitsArg>;

public:

    void encode(Encoder& encoder, const Map& map)
    {
        SourceType<decltype(m_entries)> entriesVector(map.size());
        EncoderScratchCharge entriesCharge(encoder, encoderHeapBytes(entriesVector));
        unsigned i = 0;
        for (const auto& it : map)
            entriesVector[i++] = { it.key, it.value };
        m_entries.encode(encoder, entriesVector); // in the map's order (declaration order while inline): it is the order global vars are created in

        // JITCache: a hashed map iterates in bucket order, which re-adding its entries does not reproduce, so a core also
        // writes the layout (SPEC-ucb.codec.md, E4). The native cache writes none and keeps re-adding.
        m_hashedCapacity = 0;
        Vector<uint32_t> slots;
        if (encoder.isCore() && map.usesHashedStorage()) {
            m_hashedCapacity = map.hashedCapacity();
            slots.reserveInitialCapacity(map.size());
            map.forEachOccupiedBucket([&](unsigned index, bool isDeleted, const typename Map::Entry*) {
                slots.append(index << 1 | static_cast<uint32_t>(isDeleted));
            });
        }
        EncoderScratchCharge slotsCharge(encoder, encoderHeapBytes(slots));
        m_slots.encode(encoder, slots);
    }

    void decode(Decoder& decoder, Map& map) const
    {
        SourceType<decltype(m_entries)> decodedEntries;
        m_entries.decode(decoder, decodedEntries);
        // Only a core writes a layout; the native cache's capacity is always 0.
        if (decoder.purpose() == Decoder::Purpose::JITCacheCore && m_hashedCapacity) {
            decodeHashedLayout(decoder, map, WTF::move(decodedEntries));
            return;
        }
        map.reserveInitialCapacity(decodedEntries.size());
        for (const auto& pair : decodedEntries) {
            // A malformed key decodes as the empty value, which the map cannot hold (SPEC-ucb.codec.md, E15).
            if (decoder.validates() && !coreCodecCheck(decoder, !coreCodecIsEmptyOrDeletedKey<KeyTraitsArg>(pair.first)))
                continue;
            map.add(pair.first, pair.second);
        }
    }

private:
    friend struct CoreCodecSelfTestAccess;

    // The checked restore of section 5 of SPEC-ucb.codec.md. A deleted bucket is refused before it is restored: the one map
    // the codec transports, VariableEnvironment::Map, never removes an entry, and its PackedRefPtr key cannot hold the
    // deleted value (T*)-1 its key traits give. Any failure records InconsistentMapLayout and leaves the map empty.
    void decodeHashedLayout(Decoder& decoder, Map& map, SourceType<CachedVector<CachedPair<Key, Value>>>&& decodedEntries) const
    {
        Vector<uint32_t> slots;
        m_slots.decode(decoder, slots);
        bool hasDeletedBucket = std::ranges::any_of(slots, [](uint32_t slot) { return slot & 1; });
        Vector<typename Map::Entry> entries;
        if (!hasDeletedBucket) {
            entries.reserveInitialCapacity(decodedEntries.size());
            for (auto& pair : decodedEntries) {
                // A malformed key fails the validating decode, and the restore below refuses it too.
                if (decoder.validates())
                    coreCodecCheck(decoder, !coreCodecIsEmptyOrDeletedKey<KeyTraitsArg>(pair.first));
                entries.append(typename Map::Entry { WTF::move(pair.first), WTF::move(pair.second) });
            }
        }
        if (hasDeletedBucket || !map.restoreHashedLayout(m_hashedCapacity, slots.span(), WTF::move(entries)))
            decoder.noteFailure(CoreDecodeFailure::InconsistentMapLayout);
    }

    CachedVector<CachedPair<Key, Value>> m_entries;
    uint32_t m_hashedCapacity { 0 }; // JITCache: a core's hashed map, its bucket count; 0 otherwise
    CachedVector<uint32_t> m_slots; // JITCache: (index << 1) | isDeleted for each occupied bucket, in index order
};

template<typename T>
class CachedUniquedStringImplBase : public CachedObject<T> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool decodesToCanonicalObject = true;
#endif

    // The characters follow this 12-byte header (length/flags, precomputed hash, ordinal) directly (see tailSize), instead
    // of a separately aligned allocation reached through an offset.
    static size_t tailSize(Encoder& encoder, const StringImpl& string)
    {
        Shape shape(string);
        EncoderScratchCharge copyCharge(encoder, shape.copyBytes());
        return shape.byteLength();
    }

    void encode(Encoder& encoder, const StringImpl& string)
    {
        Shape shape(string);
        EncoderScratchCharge copyCharge(encoder, shape.copyBytes());
        m_isSymbol = shape.isSymbol;
        m_isRegistered = shape.isRegistered;
        m_isWellKnownSymbol = shape.isWellKnownSymbol;
        m_isPrivate = shape.isPrivate;
        m_is8Bit = shape.characters->is8Bit();
        m_length = shape.characters->length();
        RELEASE_ASSERT(m_length == shape.characters->length()); // fits the bitfield
        m_hash = shape.characters->hash(); // what StringImpl::hash() / the atom table use, so decode never rehashes
        m_ordinal = m_isSymbol || !m_length ? noOrdinal : encoder.nextStringOrdinal(); // see Decoder::atomForOrdinal
        if (m_is8Bit)
            memcpy(tail(), shape.characters->span8().data(), shape.byteLength());
        else
            memcpy(tail(), shape.characters->span16().data(), shape.byteLength());
    }

    UniquedStringImpl* decode(Decoder& decoder) const
    {
        if (m_ordinal != noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(m_ordinal)) {
                known->ref();
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(known));
            }
        }
        auto create = [&](auto buffer) -> UniquedStringImpl* {
            if (!m_isSymbol) {
                RefPtr<AtomStringImpl> atom;
                // Long strings out of a persistent payload keep their characters in the mapping (clean, shared pages) and
                // only allocate the StringImpl header; AtomStringImpl::add adopts it in place unless the atom already exists.
                if (buffer.size() >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                    atom = AtomStringImpl::add(RefPtr<StringImpl> { StringImpl::createWithoutCopying(buffer) });
                else {
                    WTF::HashTranslatorCharBuffer<std::remove_const_t<typename decltype(buffer)::element_type>> hashed { buffer, m_hash };
                    atom = AtomStringImpl::add(hashed);
                }
                if (m_ordinal != noOrdinal)
                    decoder.setAtomForOrdinal(m_ordinal, *atom);
                return static_cast<UniquedStringImpl*>(static_cast<StringImpl*>(atom.leakRef()));
            }

            SymbolImpl* symbol;
            VM& vm = decoder.vm();
            if (m_isRegistered) {
                String str(buffer);
                if (m_isPrivate)
                    symbol = static_cast<SymbolImpl*>(&protect(vm.privateSymbolRegistry())->symbolForKey(str).leakRef());
                else
                    symbol = static_cast<SymbolImpl*>(&protect(vm.symbolRegistry())->symbolForKey(str).leakRef());
            } else {
                if (m_isWellKnownSymbol)
                    symbol = vm.propertyNames->builtinNames().lookUpWellKnownSymbol(buffer);
                else
                    symbol = vm.propertyNames->builtinNames().lookUpPrivateName(buffer);
                if (!symbol && decoder.isFallible()) {
                    // JITCache: a core naming a symbol this VM lacks fails its decode instead of the process. The null
                    // symbol is distinct per call, so no table built from it has duplicate keys (SPEC-ucb.codec.md, E5).
                    decoder.noteFailure(CoreDecodeFailure::UnresolvedSymbol);
                    return &SymbolImpl::createNullSymbol().leakRef();
                }
                RELEASE_ASSERT(symbol);
                symbol->ref();
            }
            ASSERT(m_isWellKnownSymbol != symbol->isPrivate());
            return symbol;
        };

        if (!m_length) {
            if (m_isSymbol)
                return &SymbolImpl::createNullSymbol().leakRef();
            return RefPtr { emptyAtom().impl() }.leakRef();
        }

        return m_is8Bit ? create(span8()) : create(span16());
    }

    // JITCache: the checks of the validating decode, on a record whose 12-byte header lies inside the payload
    // (SPEC-ucb.codec.md, E15).
    bool isWellFormed(Decoder& decoder) const
    {
        // The characters end inside the payload.
        size_t byteLength = static_cast<size_t>(m_length) * (m_is8Bit ? sizeof(Latin1Character) : sizeof(char16_t));
        if (!coreCodecHolds(decoder, tail(), byteLength, 1))
            return false;
        // AtomStringImpl::add takes the stored hash as given. An empty record never reaches it.
        if (m_length && m_hash != (m_is8Bit ? StringHasher::computeHashAndMaskTop8Bits(span8()) : StringHasher::computeHashAndMaskTop8Bits(span16())))
            return false;
        // A JITCacheCore encoder numbers exactly the strings that are neither symbols nor empty (NumberStrings::Yes), and
        // decode() consults the ordinal before the symbol bit.
        if ((m_ordinal == noOrdinal) != (m_isSymbol || !m_length))
            return false;
        if (m_ordinal != noOrdinal) {
            // Decoder::setAtomForOrdinal asserts that an ordinal is below the payload's size.
            if (m_ordinal >= decoder.payloadSpan().size())
                return false;
            // A record whose ordinal the decoder already holds names that atom's characters.
            if (AtomStringImpl* known = decoder.atomForOrdinal(m_ordinal)) {
                if (!(m_is8Bit ? WTF::equal(known, span8()) : WTF::equal(known, span16())))
                    return false;
            }
        }
        // decode() asserts that a registered symbol is private exactly when the record does not call it well-known.
        if (m_isSymbol && m_length && m_isRegistered && m_isWellKnownSymbol == m_isPrivate)
            return false;
        return true;
    }

    // For uses that only need the characters (a string constant's JSString), not an atom: no atom table involved.
    String decodePlainString(Decoder& decoder) const
    {
        if (m_isSymbol) {
            UniquedStringImpl* symbol = decode(decoder);
            if (!symbol)
                return String();
            return String { adoptRef(*static_cast<StringImpl*>(symbol)) };
        }
        if (!m_length)
            return emptyString();
        if (m_ordinal != noOrdinal) {
            if (AtomStringImpl* known = decoder.atomForOrdinal(m_ordinal))
                return String { known };
        }
        if (m_is8Bit) {
            if (m_length >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
                return StringImpl::createWithoutCopying(span8());
            return StringImpl::create(span8());
        }
        if (m_length >= minimumLengthToAliasPayload && decoder.canBorrowPayload())
            return StringImpl::createWithoutCopying(span16());
        return StringImpl::create(span16());
    }

    static constexpr unsigned minimumLengthToAliasPayload = 48; // below this a copy is smaller than pinning part of a page
    bool isSymbol() const { return m_isSymbol; }
    std::span<const Latin1Character> NODELETE span8() const LIFETIME_BOUND { return { std::bit_cast<const Latin1Character*>(tail()), m_length }; }
    std::span<const char16_t> NODELETE span16() const LIFETIME_BOUND { return { std::bit_cast<const char16_t*>(tail()), m_length }; }

private:
    // What is actually stored for a given string: well-known symbols are stored by their description minus "Symbol.",
    // and Latin-1 contents are stored 8-bit even if this process's atom for them happens to be 16-bit (an equal 16-bit
    // string was atomized first), since that is not a property of the source.
    struct Shape {
        explicit Shape(const StringImpl& string)
            : characters(const_cast<StringImpl*>(&string))
            , isSymbol(string.isSymbol())
        {
            if (isSymbol) {
                SymbolImpl& symbol = static_cast<SymbolImpl&>(*characters);
                isRegistered = symbol.isRegistered();
                isPrivate = symbol.isPrivate();
                if (!symbol.isNullSymbol() && !isPrivate) {
                    isWellKnownSymbol = true;
                    characters = symbol.substring(strlen("Symbol."));
                }
            }
            if (!characters->is8Bit() && WTF::charactersAreAllLatin1(characters->span16()))
                characters = StringImpl::create8BitIfPossible(characters->span16());
        }
        size_t byteLength() const { return characters->length() * (characters->is8Bit() ? 1 : 2); }
        // JITCache: what the shape allocated for a string it does not store as given, which an encode charges (E8).
        size_t copyBytes() const { return characters.get() == original ? 0 : sizeof(StringImpl) + byteLength(); }
        RefPtr<StringImpl> characters;
        const StringImpl* original { characters.get() };
        bool isSymbol { false };
        bool isRegistered { false };
        bool isWellKnownSymbol { false };
        bool isPrivate { false };
    };
    friend struct CoreCodecSelfTestAccess;

    const uint8_t* tail() const { return std::bit_cast<const uint8_t*>(this + 1); }
    uint8_t* tail() { return std::bit_cast<uint8_t*>(this + 1); }
    uint32_t m_length : 27;
    uint32_t m_is8Bit : 1;
    uint32_t m_isSymbol : 1;
    uint32_t m_isWellKnownSymbol : 1;
    uint32_t m_isRegistered : 1;
    uint32_t m_isPrivate : 1;
    uint32_t m_hash { 0 };
    // Distinct (non-symbol) strings are numbered in encode order; the decoder keeps the atom for each number it has seen,
    // so only the first block to name a string goes through the atom table.
    static constexpr uint32_t noOrdinal = std::numeric_limits<uint32_t>::max();
    uint32_t m_ordinal { noOrdinal };
};
class CachedUniquedStringImpl : public CachedUniquedStringImplBase<UniquedStringImpl> { };
class CachedStringImpl : public CachedUniquedStringImplBase<StringImpl> { };

class CachedString : public CachedObject<String> {
public:
    void encode(Encoder& encoder, const String& string)
    {
        m_impl.encode(encoder, static_cast<UniquedStringImpl*>(string.impl()));
    }

    String decode(Decoder& decoder) const
    {
        return String(static_cast<RefPtr<StringImpl>>(m_impl.decode(decoder)));
    }

    void decode(Decoder& decoder, String& dst) const
    {
        dst = decode(decoder);
    }

    // JITCache: a pattern or a template literal's raw string, which a JITCacheCore encoder never writes null.
    bool isNull() const { return m_impl.ptr().isEmpty(); }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedRefPtr<CachedUniquedStringImpl> m_impl;
};

class CachedIdentifier : public CachedObject<Identifier> {
public:
    void encode(Encoder& encoder, const Identifier& identifier)
    {
        m_string.encode(encoder, identifier.string());
    }

    Identifier decode(Decoder& decoder) const
    {
        String str = m_string.decode(decoder);
        if (str.isNull())
            return Identifier();

        return Identifier::fromUid(decoder.vm(), (UniquedStringImpl*)str.impl());
    }

    void decode(Decoder& decoder, Identifier& ident) const
    {
        ident = decode(decoder);
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedString m_string;
};

template<typename T>
class CachedOptional : public VariableLengthObject<std::optional<SourceType<T>>> {
public:
    void encode(Encoder& encoder, const std::optional<SourceType<T>>& source)
    {
        if (!source)
            return;

        this->template allocateFor<T>(encoder, *source)->encode(encoder, *source);
    }

    std::optional<SourceType<T>> decode(Decoder& decoder) const
    {
        if (this->isEmpty())
            return std::nullopt;

        if (decoder.validates()) {
            const T* record = this->template placedTarget<T>(decoder);
            if (!coreCodecCheck(decoder, record && coreCodecIsWellFormed(decoder, *record)))
                return std::nullopt;
            return { record->decode(decoder) };
        }
        return { this->template buffer<T>()->decode(decoder) };
    }

    void decode(Decoder& decoder, std::optional<SourceType<T>>& dst) const
    {
        dst = decode(decoder);
    }

    void encode(Encoder& encoder, const std::unique_ptr<SourceType<T>>& source)
    {
        if (!source)
            encode(encoder, std::nullopt);
        else
            encode(encoder, { *source });
    }

    SourceType<T>* decodeAsPtr(Decoder& decoder) const
    {
        if (decoder.validates()) {
            // The slot must be filled (SPEC-ucb.codec.md, E15).
            const T* record = this->template placedTarget<T>(decoder);
            if (!coreCodecCheck(decoder, record && coreCodecIsWellFormed(decoder, *record)))
                return nullptr;
            return record->decode(decoder);
        }
        RELEASE_ASSERT(!this->isEmpty());
        return this->template buffer<T>()->decode(decoder);
    }
};

class CachedSimpleJumpTable : public CachedObject<UnlinkedSimpleJumpTable> {
public:
    void encode(Encoder& encoder, const UnlinkedSimpleJumpTable& jumpTable)
    {
        m_min = jumpTable.m_min;
        m_defaultOffset = jumpTable.m_defaultOffset;
        m_isList = jumpTable.m_isList;
        m_branchOffsets.encode(encoder, jumpTable.m_branchOffsets);
    }

    void decode(Decoder& decoder, UnlinkedSimpleJumpTable& jumpTable) const
    {
        jumpTable.m_min = m_min;
        jumpTable.m_defaultOffset = m_defaultOffset;
        jumpTable.m_isList = m_isList;
        m_branchOffsets.decode(decoder, jumpTable.m_branchOffsets);
    }

private:
    int32_t m_min;
    int32_t m_defaultOffset;
    int32_t m_isList;
    CachedVector<int32_t> m_branchOffsets;
};

class CachedStringJumpTable : public CachedObject<UnlinkedStringJumpTable> {
public:
    void encode(Encoder& encoder, const UnlinkedStringJumpTable& jumpTable)
    {
        m_offsetTable.encode(encoder, jumpTable.m_offsetTable);
        m_minLength = jumpTable.m_minLength;
        m_maxLength = jumpTable.m_maxLength;
        m_defaultOffset = jumpTable.m_defaultOffset;
    }

    void decode(Decoder& decoder, UnlinkedStringJumpTable& jumpTable) const
    {
        m_offsetTable.decode(decoder, jumpTable.m_offsetTable);
        jumpTable.m_minLength = m_minLength;
        jumpTable.m_maxLength = m_maxLength;
        jumpTable.m_defaultOffset = m_defaultOffset;
    }

private:
    CachedMemoryCompactLookupOnlyRobinHoodHashMap<CachedRefPtr<CachedStringImpl>, UnlinkedStringJumpTable::OffsetLocation> m_offsetTable;
    unsigned m_minLength { 0 };
    unsigned m_maxLength { 0 };
    int32_t m_defaultOffset { 0 };
};

class CachedBitVector : public VariableLengthObject<BitVector> {
public:
    void encode(Encoder& encoder, const BitVector& bitVector)
    {
        m_numBits = safeCast<uint32_t>(bitVector.size());
        if (!m_numBits)
            return;
        size_t sizeInBytes = BitVector::byteCount(m_numBits);
        uint8_t* buffer = this->allocate(encoder, sizeInBytes, alignof(uintptr_t));
        memcpy(buffer, bitVector.words().data(), sizeInBytes);
    }

    void decode(Decoder& decoder, BitVector& bitVector) const
    {
        if (!m_numBits)
            return;
        size_t sizeInBytes = BitVector::byteCount(m_numBits);
        const uint8_t* bits = decoder.validates() ? this->placedBytes(decoder, sizeInBytes, alignof(uintptr_t)) : this->buffer();
        if (!coreCodecCheck(decoder, !!bits))
            return;
        bitVector.ensureSize(m_numBits);
        memcpy(bitVector.words().data(), bits, sizeInBytes);
    }

private:
    uint32_t m_numBits;
};

template<typename T, typename HashArg = DefaultHash<T>>
class CachedHashSet : public CachedObject<UncheckedKeyHashSet<SourceType<T>, HashArg>> {
public:
    void encode(Encoder& encoder, const UncheckedKeyHashSet<SourceType<T>, HashArg>& set)
    {
        SourceType<decltype(m_entries)> entriesVector(set.size());
        EncoderScratchCharge entriesCharge(encoder, encoderHeapBytes(entriesVector));
        unsigned i = 0;
        for (const auto& item : set)
            entriesVector[i++] = item;
        EncodingOrder::sort(entriesVector, [](const auto& item) -> const auto& { return item; });
        m_entries.encode(encoder, entriesVector);
    }

    void decode(Decoder& decoder, UncheckedKeyHashSet<SourceType<T>, HashArg>& set) const
    {
        SourceType<decltype(m_entries)> entriesVector;
        m_entries.decode(decoder, entriesVector);
        for (const auto& item : entriesVector) {
            // A malformed key decodes as the empty value, which the set cannot hold (SPEC-ucb.codec.md, E15).
            if (decoder.validates() && !coreCodecCheck(decoder, !coreCodecIsEmptyOrDeletedKey<HashTraits<SourceType<T>>>(item)))
                continue;
            set.add(item);
        }
    }

private:
    CachedVector<T> m_entries;
};

// UnlinkedHandlerInfo keeps its HandlerType in a 2-bit bit-field; the other 30 bits would be whatever the heap held.
class CachedHandlerInfo : public CachedObject<UnlinkedHandlerInfo> {
public:
    void encode(Encoder&, const UnlinkedHandlerInfo& handlerInfo)
    {
        m_start = handlerInfo.start;
        m_end = handlerInfo.end;
        m_target = handlerInfo.target;
        m_type = static_cast<uint32_t>(handlerInfo.type());
    }

    void decode(Decoder& decoder, UnlinkedHandlerInfo& handlerInfo) const
    {
        // The type is cast to HandlerType (SPEC-ucb.codec.md, E15); a malformed one leaves the default handler.
        if (decoder.validates() && !coreCodecCheck(decoder, m_type <= static_cast<uint32_t>(HandlerType::SynthesizedFinally)))
            return;
        handlerInfo = UnlinkedHandlerInfo(m_start, m_end, m_target, static_cast<HandlerType>(m_type));
    }

private:
    uint32_t m_start;
    uint32_t m_end;
    uint32_t m_target;
    uint32_t m_type;
};

class CachedCodeBlockRareData : public CachedObject<UnlinkedCodeBlock::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const UnlinkedCodeBlock::RareData& rareData)
    {
        m_exceptionHandlers.encode(encoder, rareData.m_exceptionHandlers);
        m_unlinkedSwitchJumpTables.encode(encoder, rareData.m_unlinkedSwitchJumpTables);
        m_unlinkedStringSwitchJumpTables.encode(encoder, rareData.m_unlinkedStringSwitchJumpTables);
        m_typeProfilerInfoMap.encode(encoder, rareData.m_typeProfilerInfoMap);
        m_opProfileControlFlowBytecodeOffsets.encode(encoder, rareData.m_opProfileControlFlowBytecodeOffsets);
        m_bitVectors.encode(encoder, rareData.m_bitVectors);
        m_constantIdentifierSets.encode(encoder, rareData.m_constantIdentifierSets);
        m_needsClassFieldInitializer = rareData.m_needsClassFieldInitializer;
        m_privateBrandRequirement = rareData.m_privateBrandRequirement;
    }

    UnlinkedCodeBlock::RareData* decode(Decoder& decoder) const
    {
        UnlinkedCodeBlock::RareData* rareData = new UnlinkedCodeBlock::RareData { };
        m_exceptionHandlers.decode(decoder, rareData->m_exceptionHandlers);
        m_unlinkedSwitchJumpTables.decode(decoder, rareData->m_unlinkedSwitchJumpTables);
        m_unlinkedStringSwitchJumpTables.decode(decoder, rareData->m_unlinkedStringSwitchJumpTables);
        m_typeProfilerInfoMap.decode(decoder, rareData->m_typeProfilerInfoMap);
        m_opProfileControlFlowBytecodeOffsets.decode(decoder, rareData->m_opProfileControlFlowBytecodeOffsets);
        m_bitVectors.decode(decoder, rareData->m_bitVectors);
        m_constantIdentifierSets.decode(decoder, rareData->m_constantIdentifierSets);
        rareData->m_needsClassFieldInitializer = m_needsClassFieldInitializer;
        rareData->m_privateBrandRequirement = m_privateBrandRequirement;
        return rareData;
    }

private:
    CachedVector<CachedHandlerInfo> m_exceptionHandlers;
    CachedVector<CachedSimpleJumpTable> m_unlinkedSwitchJumpTables;
    CachedVector<CachedStringJumpTable> m_unlinkedStringSwitchJumpTables;
    CachedHashMap<unsigned, UnlinkedCodeBlock::RareData::TypeProfilerExpressionRange> m_typeProfilerInfoMap;
    CachedVector<JSInstructionStream::Offset> m_opProfileControlFlowBytecodeOffsets;
    CachedVector<CachedBitVector> m_bitVectors;
    CachedVector<CachedHashSet<CachedRefPtr<CachedUniquedStringImpl>, IdentifierRepHash>> m_constantIdentifierSets;
    unsigned m_needsClassFieldInitializer : 1;
    unsigned m_privateBrandRequirement : 1;
    unsigned m_unused : 30 { 0 };
};

// [u32 numberOfEncodedInfo][u8 flags][varint chapters][varint extensions][pad to 4][payload words][u32 checksum if flagged]
// Self-contained and position-independent, so identical ones (every async wrapper, say) are written once.
class CachedExpressionInfo : public CachedObject<ExpressionInfo> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif
    enum Flag : uint8_t { HasChecksum = 1 << 0 };

    static Vector<uint8_t, 64> pack(const ExpressionInfo& info, bool checksum)
    {
        VarintWriter head;
        head.u8(checksum ? HasChecksum : 0);
        head.u32(info.m_numberOfChapters);
        head.u32(info.m_numberOfEncodedInfoExtensions);
        size_t payloadAt = roundUpToMultipleOf<4>(sizeof(uint32_t) + head.size());
        size_t payloadBytes = info.payloadSize() * sizeof(unsigned);
        Vector<uint8_t, 64> bytes;
        bytes.grow(payloadAt + payloadBytes + (checksum ? sizeof(uint32_t) : 0));
        memset(bytes.mutableSpan().data(), 0, bytes.size());
        uint32_t count = info.m_numberOfEncodedInfo;
        memcpy(bytes.mutableSpan().data(), &count, sizeof(count));
        head.copyTo(bytes.mutableSpan().data() + sizeof(uint32_t));
        if (payloadBytes)
            memcpy(bytes.mutableSpan().data() + payloadAt, info.payload(), payloadBytes);
        if (checksum) {
            uint32_t crc = ~crc32c(~0u, bytes.span().first(payloadAt + payloadBytes));
            memcpy(bytes.mutableSpan().data() + payloadAt + payloadBytes, &crc, sizeof(crc));
        }
        return bytes;
    }

    // A damaged one decodes as "no expression info" (stack traces lose line/column for that function) rather than failing the function.
    // A validating decode records the damage too (SPEC-ucb.codec.md, E15), and a JITCacheCore encoder sets no flag.
    std::unique_ptr<ExpressionInfo> decode(Decoder& decoder) const
    {
        const uint8_t* base = std::bit_cast<const uint8_t*>(this);
        auto payload = decoder.payloadSpan();
        const uint8_t* limit = payload.data() + payload.size();
        auto damaged = [&] {
            if (decoder.validates())
                decoder.noteFailure(CoreDecodeFailure::Malformed);
            return ExpressionInfo::createUninitialized(0, 0, 0);
        };
        if (!decoder.payloadContains(base, sizeof(uint32_t) + 1))
            return damaged();
        if (decoder.validates() && !coreCodecHolds(decoder, base, sizeof(CachedExpressionInfo), alignof(CachedExpressionInfo)))
            return damaged();
        VarintReader reader(base + sizeof(uint32_t), limit);
        uint8_t flags = reader.u8();
        unsigned chapters = reader.u32();
        unsigned extensions = reader.u32();
        if (reader.overran() || (decoder.validates() && flags))
            return damaged();
        unsigned encodedInfo = m_numberOfEncodedInfo;
        size_t payloadAt = roundUpToMultipleOf<4>(reader.position() - base);
        size_t payloadBytes = ExpressionInfo::payloadSizeInBytes(chapters, encodedInfo, extensions);
        size_t total = payloadAt + payloadBytes + ((flags & HasChecksum) ? sizeof(uint32_t) : 0);
        if (!decoder.payloadContains(base, total))
            return damaged();
        if ((flags & HasChecksum) && decoder.verifiesChecksums()) {
            uint32_t stored;
            memcpy(&stored, base + payloadAt + payloadBytes, sizeof(stored));
            if (stored != ~crc32c(~0u, std::span { base, payloadAt + payloadBytes })) {
                dataLogLnIf(Options::verboseDiskCache(), "[Disk Cache] expression info checksum mismatch; dropping it");
                return ExpressionInfo::createUninitialized(0, 0, 0);
            }
        }
        const unsigned* words = reinterpret_cast<const unsigned*>(base + payloadAt);
        if (decoder.canBorrowPayload() && payloadBytes)
            return ExpressionInfo::createBorrowed(chapters, encodedInfo, extensions, words);
        auto info = ExpressionInfo::createUninitialized(chapters, encodedInfo, extensions);
        if (payloadBytes)
            memcpy(info->payload(), words, payloadBytes);
        return info;
    }

private:
    uint32_t m_numberOfEncodedInfo;
};
static_assert(sizeof(CachedExpressionInfo) == sizeof(uint32_t) && alignof(CachedExpressionInfo) == 4);

// VariableEnvironmentEntry and PrivateNameEntry are 16 bits; held in 32 so the pairs that hold them have no padding.
template<typename Entry>
class CachedEntryBits : public CachedObject<Entry> {
public:
    void encode(Encoder&, const Entry& entry) { m_bits = std::bit_cast<uint16_t>(entry); }
    void decode(Decoder&, Entry& entry) const { entry = std::bit_cast<Entry>(static_cast<uint16_t>(m_bits)); }
    Entry decode(Decoder&) const { return std::bit_cast<Entry>(static_cast<uint16_t>(m_bits)); }

private:
    uint32_t m_bits;
};

typedef CachedHashMap<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>, CachedEntryBits<PrivateNameEntry>, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, PrivateNameEntryHashTraits> CachedPrivateNameEnvironment;

class CachedVariableEnvironmentRareData : public CachedObject<VariableEnvironment::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const VariableEnvironment::RareData& rareData)
    {
        m_privateNames.encode(encoder, rareData.m_privateNames);
    }

    void decode(Decoder& decoder, VariableEnvironment::RareData& rareData) const
    {
        m_privateNames.decode(decoder, rareData.m_privateNames);
    }

private:
    CachedPrivateNameEnvironment m_privateNames;
};

class CachedVariableEnvironment : public CachedObject<VariableEnvironment> {
public:
    void encode(Encoder& encoder, const VariableEnvironment& env)
    {
        m_isEverythingCaptured = env.m_isEverythingCaptured;
        m_hasAwaitUsingDeclaration = env.m_hasAwaitUsingDeclaration;
        m_map.encode(encoder, env.m_map);
        m_rareData.encode(encoder, env.m_rareData.get());
    }

    void decode(Decoder& decoder, VariableEnvironment& env) const
    {
        env.m_isEverythingCaptured = m_isEverythingCaptured;
        env.m_hasAwaitUsingDeclaration = m_hasAwaitUsingDeclaration;
        m_map.decode(decoder, env.m_map);
        if (!m_rareData.isEmpty()) {
            const CachedVariableEnvironmentRareData* rareData = decoder.validates() ? m_rareData.placedTarget<CachedVariableEnvironmentRareData>(decoder) : m_rareData.operator->();
            if (!coreCodecCheck(decoder, !!rareData))
                return;
            env.m_rareData = WTF::makeUnique<VariableEnvironment::RareData>();
            rareData->decode(decoder, *env.m_rareData);
        }
    }

private:
    friend struct CoreCodecSelfTestAccess;

    bool m_isEverythingCaptured;
    bool m_hasAwaitUsingDeclaration;
    uint8_t m_unused[2] { };
    CachedInlineMap<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>, CachedEntryBits<VariableEnvironmentEntry>, VariableEnvironment::inlineMapCapacity, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits> m_map;
    CachedPtr<CachedVariableEnvironmentRareData> m_rareData;
};

class CachedCompactTDZEnvironment : public CachedObject<CompactTDZEnvironment> {
public:
    void encode(Encoder& encoder, const CompactTDZEnvironment& env)
    {
        // A Compact is sorted by StringImpl address; decode() sorts again.
        CompactTDZEnvironment::Compact compact;
        if (std::holds_alternative<CompactTDZEnvironment::Compact>(env.m_variables))
            compact = std::get<CompactTDZEnvironment::Compact>(env.m_variables);
        else {
            for (auto& key : std::get<CompactTDZEnvironment::Inflated>(env.m_variables))
                compact.append(key);
        }
        EncoderScratchCharge compactCharge(encoder, encoderHeapBytes(compact));
        EncodingOrder::sort(compact, [](const auto& key) -> const auto& { return key; });
        m_variables.encode(encoder, compact);
        m_hash = env.m_hash;
    }

    void decode(Decoder& decoder, CompactTDZEnvironment& env) const
    {
        {
            CompactTDZEnvironment::Compact compact;
            m_variables.decode(decoder, compact);
            CompactTDZEnvironment::sortCompact(compact);
            env.m_variables = CompactTDZEnvironment::Variables(WTF::move(compact));
        }
        env.m_hash = m_hash;
    }

    CompactTDZEnvironment* decode(Decoder& decoder) const
    {
        if (decoder.validates()) {
            // A malformed name decodes as null, which CompactTDZEnvironment cannot hold, and the VM interns an environment
            // by the hash it is given, the exclusive-or of its names' hashes (SPEC-ucb.codec.md, E15).
            CompactTDZEnvironment::Compact compact;
            m_variables.decode(decoder, compact);
            unsigned hash = 0;
            for (auto& name : compact) {
                if (!coreCodecCheck(decoder, !!name))
                    return nullptr;
                hash ^= name->hash();
            }
            if (!coreCodecCheck(decoder, hash == m_hash))
                return nullptr;
            CompactTDZEnvironment::sortCompact(compact);
            CompactTDZEnvironment* env = new CompactTDZEnvironment;
            env->m_variables = CompactTDZEnvironment::Variables(WTF::move(compact));
            env->m_hash = m_hash;
            return env;
        }
        CompactTDZEnvironment* env = new CompactTDZEnvironment;
        decode(decoder, *env);
        return env;
    }

    // JITCache: the environment digest of SPEC-ucb.md section 3.4, computed on the VM thread when a holder digest first
    // reaches the environment and kept in it until it frees its names. The budget is charged for the sort buffer while it
    // lives; on a refusal the digest is empty and the environment keeps none.
    static std::optional<std::array<uint8_t, 32>> contentDigest(const CompactTDZEnvironment& environment, CoreEncodingBudget* budget, unsigned& environmentsDigested)
    {
        if (environment.m_contentDigest)
            return *environment.m_contentDigest;

        size_t count = WTF::switchOn(environment.m_variables,
            [](const CompactTDZEnvironment::Compact& compact) -> size_t { return compact.size(); },
            [](const CompactTDZEnvironment::Inflated& inflated) -> size_t { return inflated.size(); });
        size_t sortBufferBytes = count * sizeof(const UniquedStringImpl*);
        if (budget && sortBufferBytes && !budget->charge(sortBufferBytes))
            return std::nullopt;
        std::array<uint8_t, 32> digest;
        {
            auto releaseSortBuffer = makeScopeExit([&] {
                if (budget && sortBufferBytes)
                    budget->release(sortBufferBytes);
            });
            Vector<const UniquedStringImpl*> names;
            names.reserveInitialCapacity(count);
            WTF::switchOn(environment.m_variables,
                [&](const CompactTDZEnvironment::Compact& compact) {
                    for (auto& name : compact)
                        names.append(name.get());
                },
                [&](const CompactTDZEnvironment::Inflated& inflated) {
                    for (auto& name : inflated)
                        names.append(name.get());
                });
            EncodingOrder::sort(names, [](const UniquedStringImpl* name) -> const StringImpl* { return name; });

            JITCache::SHA256 hasher;
            hasher.update("JITCache.tdze.v1"_s.span8());
            coreCodecHashU32(hasher, static_cast<uint32_t>(names.size()));
            for (const UniquedStringImpl* name : names) {
                uint8_t kind = static_cast<uint8_t>(EncodingOrder::kind(name));
                hasher.update(std::span { &kind, 1 });
                coreCodecHashCanonicalString(hasher, name);
            }
            digest = hasher.finalize();
        }
        const_cast<CompactTDZEnvironment&>(environment).m_contentDigest = makeUniqueWithoutFastMallocCheck<std::array<uint8_t, 32>>(digest);
        ++environmentsDigested;
        return digest;
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedVector<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>> m_variables;
    unsigned m_hash;
};

class CachedCompactTDZEnvironmentMapHandle : public CachedObject<CompactTDZEnvironmentMap::Handle> {
public:
    void encode(Encoder& encoder, const CompactTDZEnvironmentMap::Handle& handle)
    {
        m_environment.encode(encoder, handle.m_environment);
    }

    CompactTDZEnvironmentMap::Handle decode(Decoder& decoder) const
    {
        bool isNewAllocation;
        CompactTDZEnvironment* environment = m_environment.decode(decoder, isNewAllocation);
        if (!environment) {
            ASSERT(!isNewAllocation);
            return CompactTDZEnvironmentMap::Handle();
        }

        if (!isNewAllocation)
            return decoder.handleForTDZEnvironment(environment);
        bool isNewEntry;
        CompactTDZEnvironmentMap::Handle handle = decoder.vm().m_compactVariableMap->get(environment, isNewEntry);
        if (!isNewEntry) {
            decoder.addFinalizer([=] {
                delete environment;
            });
        }
        decoder.setHandleForTDZEnvironment(environment, handle);
        return handle;
    }

    void decode(Decoder& decoder, CompactTDZEnvironmentMap::Handle& handle) const
    {
        handle = decode(decoder);
    }

    // JITCache: the start record of a core has an empty handle, which no native link has (SPEC-ucb.codec.md, E14).
    bool isEmpty() const { return m_environment.isEmpty(); }

    // The interned environment a live handle holds; null for an empty handle.
    static const CompactTDZEnvironment* environmentOf(const CompactTDZEnvironmentMap::Handle& handle) { return handle.m_environment; }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedPtr<CachedCompactTDZEnvironment> m_environment;
};

class CachedScopedArgumentsTable : public CachedObject<ScopedArgumentsTable> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const ScopedArgumentsTable& scopedArgumentsTable)
    {
        m_length = scopedArgumentsTable.m_arguments.size();
        m_arguments.encode(encoder, scopedArgumentsTable.m_arguments.span().data(), m_length);
    }

    ScopedArgumentsTable* decode(Decoder& decoder) const
    {
        // ScopedArgumentsTable::tryCreate needs a length whose offsets the payload holds (SPEC-ucb.codec.md, E15).
        if (decoder.validates() && m_length && !coreCodecCheck(decoder, !!m_arguments.placedTarget<ScopeOffset>(decoder, m_length)))
            return nullptr;
        ScopedArgumentsTable* scopedArgumentsTable = ScopedArgumentsTable::tryCreate(decoder.vm(), m_length);
        RELEASE_ASSERT(scopedArgumentsTable); // We crash here. This is unlikely to continue execution if we hit this condition when decoding UnlinkedCodeBlock.
        m_arguments.decode(decoder, scopedArgumentsTable->m_arguments.mutableSpan().data(), m_length);
        return scopedArgumentsTable;
    }

private:
    uint32_t m_length;
    CachedArray<ScopeOffset> m_arguments;
};

class CachedSymbolTableEntry : public CachedObject<SymbolTableEntry> {
public:
    // A slim entry is a 32-bit raw VarOffset above six flag bits. Offsets are far below 2^25, so the two fit in one word.
    void encode(Encoder&, const SymbolTableEntry& symbolTableEntry)
    {
        intptr_t bits = symbolTableEntry.bits() | SymbolTableEntry::SlimFlag;
        int32_t rawOffset = static_cast<int32_t>(bits >> SymbolTableEntry::FlagBits);
        RELEASE_ASSERT((rawOffset << SymbolTableEntry::FlagBits) >> SymbolTableEntry::FlagBits == rawOffset);
        m_bits = (rawOffset << SymbolTableEntry::FlagBits) | static_cast<int32_t>(bits & ((1 << SymbolTableEntry::FlagBits) - 1));
        ASSERT(unpack() == bits);
    }

    void decode(Decoder& decoder, SymbolTableEntry& symbolTableEntry) const
    {
        // A fat entry's bits are a pointer, which a payload cannot hold (SPEC-ucb.codec.md, E15); a malformed entry keeps the
        // default slim entry.
        if (decoder.validates() && !coreCodecCheck(decoder, !!(unpack() & SymbolTableEntry::SlimFlag)))
            return;
        symbolTableEntry.m_bits = unpack();
    }

private:
    friend struct CoreCodecSelfTestAccess;

    static constexpr int32_t slimFlag() { return SymbolTableEntry::SlimFlag; }

    intptr_t unpack() const
    {
        unsigned rawOffset = static_cast<unsigned>(m_bits >> SymbolTableEntry::FlagBits);
        return (static_cast<intptr_t>(rawOffset) << SymbolTableEntry::FlagBits) | (m_bits & ((1 << SymbolTableEntry::FlagBits) - 1));
    }

    int32_t m_bits;
};

class CachedSymbolTableRareData : public CachedObject<SymbolTable::SymbolTableRareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    void encode(Encoder& encoder, const SymbolTable::SymbolTableRareData& rareData)
    {
        m_privateNames.encode(encoder, rareData.m_privateNames);
    }

    void decode(Decoder& decoder, SymbolTable::SymbolTableRareData& rareData) const
    {
        m_privateNames.decode(decoder, rareData.m_privateNames);
    }

private:
    CachedPrivateNameEnvironment m_privateNames;
};

class CachedSymbolTable : public CachedObject<SymbolTable> {
public:
    void encode(Encoder& encoder, const SymbolTable& symbolTable)
    {
        m_map.encode(encoder, symbolTable.m_map);
        m_maxScopeOffset = symbolTable.m_maxScopeOffset;
        m_usesSloppyEval = symbolTable.m_usesSloppyEval;
        m_nestedLexicalScope = symbolTable.m_nestedLexicalScope;
        m_scopeType = symbolTable.m_scopeType;
        m_arguments.encode(encoder, symbolTable.m_arguments.get());
        m_rareData.encode(encoder, symbolTable.m_rareData.get());
    }

    SymbolTable* decode(Decoder& decoder) const
    {
        SymbolTable* symbolTable = SymbolTable::create(decoder.vm());
        m_map.decode(decoder, symbolTable->m_map);
        symbolTable->m_maxScopeOffset = m_maxScopeOffset;
        symbolTable->m_usesSloppyEval = m_usesSloppyEval;
        symbolTable->m_nestedLexicalScope = m_nestedLexicalScope;
        // The scope type is read back as SymbolTable::ScopeType (SPEC-ucb.codec.md, E15).
        if (!decoder.validates() || coreCodecCheck(decoder, m_scopeType <= SymbolTable::FunctionNameScope))
            symbolTable->m_scopeType = m_scopeType;
        ScopedArgumentsTable* scopedArgumentsTable = m_arguments.decode(decoder);
        if (scopedArgumentsTable)
            symbolTable->m_arguments.set(decoder.vm(), symbolTable, scopedArgumentsTable);
        if (!m_rareData.isEmpty()) {
            const CachedSymbolTableRareData* rareData = decoder.validates() ? m_rareData.placedTarget<CachedSymbolTableRareData>(decoder) : m_rareData.operator->();
            if (!coreCodecCheck(decoder, !!rareData))
                return symbolTable;
            symbolTable->m_rareData = WTF::makeUnique<SymbolTable::SymbolTableRareData>();
            rareData->decode(decoder, *symbolTable->m_rareData);
        }

        return symbolTable;
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedHashMap<CachedRefPtr<CachedUniquedStringImpl>, CachedSymbolTableEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, SymbolTableIndexHashTraits> m_map;
    ScopeOffset m_maxScopeOffset;
    unsigned m_usesSloppyEval : 1;
    unsigned m_nestedLexicalScope : 1;
    unsigned m_scopeType : 3;
    unsigned m_unused : 27 { 0 };
    CachedPtr<CachedScopedArgumentsTable> m_arguments;
    CachedPtr<CachedSymbolTableRareData> m_rareData;
};

// Whose constants a pool holds: a code block's constant registers, or an immutable butterfly's elements.
enum class CachedJSValuePoolOwner : uint8_t { CodeBlock, Butterfly };

// A pool reached through a slot-relative offset (constant arrays in a butterfly).
class CachedJSValuePoolRef : public VariableLengthObject<WriteBarrier<Unknown>*> {
public:
    void encode(Encoder&, std::span<const WriteBarrier<Unknown>>);
    void decode(Decoder&, WriteBarrier<Unknown>* out, unsigned count, const JSCell* owner) const;
    // JITCache: whether the pool of `count` constants lies inside the payload (SPEC-ucb.codec.md, E15).
    bool isPlaced(Decoder&, unsigned count) const;
    // JITCache: the kind bytes of a placed pool of `count` constants.
    std::span<const uint8_t> kinds(unsigned count) const
    {
        if (!count)
            return { };
        return { this->buffer(), count };
    }
};

class CachedJSValue;
class CachedImmutableButterfly : public CachedObject<JSCellButterfly> {
public:
    CachedImmutableButterfly()
        : m_cachedDoubles()
    {
    }

    void encode(Encoder& encoder, JSCellButterfly& immutableButterfly)
    {
        m_length = immutableButterfly.length();
        m_indexingType = immutableButterfly.indexingMode(); // not indexingTypeAndMisc(): the rest of that byte is cell-lock state
        if (hasDouble(m_indexingType))
            m_cachedDoubles.encode(encoder, immutableButterfly.toButterfly()->contiguousDouble().data(), m_length);
        else
            m_cachedValues.encode(encoder, std::span<const WriteBarrier<Unknown>> { immutableButterfly.toButterfly()->contiguous().data(), m_length });
    }

    JSCellButterfly* decode(Decoder&) const;

private:
    friend struct CoreCodecSelfTestAccess;

    bool hasWellFormedElements(Decoder&) const;

    IndexingType m_indexingType;
    uint8_t m_unused[3] { };
    unsigned m_length;
    union {
        CachedArray<double> m_cachedDoubles;
        CachedJSValuePoolRef m_cachedValues;
    };
};

class CachedRegExp : public CachedObject<RegExp> {
public:
    void encode(Encoder& encoder, const RegExp& regExp)
    {
        m_patternString.encode(encoder, regExp.m_patternString);
        m_flags = regExp.m_flags;
        // JITCache: a core writes the pattern and the flags only, so that decoding takes RegExp::create, the call generation
        // makes. The rest is compilation state of a cell the VM shares, which code deletion clears (SPEC-ucb.codec.md, E11).
        if (encoder.isCore())
            return;
        m_atom.encode(encoder, regExp.m_atom);
        m_specificPattern = regExp.m_specificPattern;
        // What RegExp::finishCreation learns from parsing the pattern, so decode can skip the parse. A pattern with named
        // groups (rare) or very many subpatterns still parses on decode.
        m_parsed = regExp.isValid() && !regExp.m_rareData && regExp.m_numSubpatterns <= std::numeric_limits<uint16_t>::max();
        m_numSubpatterns = m_parsed ? regExp.m_numSubpatterns : 0;
    }

    RegExp* decode(Decoder& decoder) const
    {
        if (decoder.validates()) {
            // The record E11 writes, a pattern, and flags within Yarr::Flags (SPEC-ucb.codec.md, E15).
            bool isCoreRecord = !m_parsed && !m_numSubpatterns && m_atom.isNull() && m_specificPattern == Yarr::SpecificPattern::None;
            bool hasKnownFlags = m_flags.toRaw() < (1u << Yarr::numberOfFlags);
            if (!coreCodecCheck(decoder, isCoreRecord && hasKnownFlags && !m_patternString.isNull()))
                return nullptr;
        }
        String pattern { m_patternString.decode(decoder) };
        if (decoder.validates() && !coreCodecCheck(decoder, !pattern.isNull()))
            return nullptr;
        if (!m_parsed)
            return RegExp::create(decoder.vm(), pattern, m_flags);
        return RegExp::createFromCache(decoder.vm(), pattern, m_flags, m_numSubpatterns, String { m_atom.decode(decoder) }, m_specificPattern);
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedString m_patternString;
    CachedString m_atom;
    uint16_t m_numSubpatterns { 0 };
    OptionSet<Yarr::Flags> m_flags;
    Yarr::SpecificPattern m_specificPattern { Yarr::SpecificPattern::None };
    uint8_t m_parsed { false };
    uint8_t m_unused[2] { };
};

class CachedTemplateObjectDescriptor : public CachedObject<TemplateObjectDescriptor> {
public:
    void encode(Encoder& encoder, const JSTemplateObjectDescriptor& descriptor)
    {
        m_rawStrings.encode(encoder, descriptor.descriptor().rawStrings());
        m_cookedStrings.encode(encoder, descriptor.descriptor().cookedStrings());
        m_endOffset = descriptor.endOffset();
    }

    JSTemplateObjectDescriptor* decode(Decoder& decoder) const
    {
        TemplateObjectDescriptor::StringVector decodedRawStrings;
        TemplateObjectDescriptor::OptionalStringVector decodedCookedStrings;
        m_rawStrings.decode(decoder, decodedRawStrings);
        m_cookedStrings.decode(decoder, decodedCookedStrings);
        if (decoder.validates()) {
            // TemplateObjectDescriptor hashes every raw string, and its cooked strings pair with them (SPEC-ucb.codec.md, E15).
            bool hasEveryRawString = std::ranges::none_of(decodedRawStrings, [](const String& string) { return string.isNull(); });
            if (!coreCodecCheck(decoder, hasEveryRawString && decodedRawStrings.size() == decodedCookedStrings.size()))
                return nullptr;
        }
        return JSTemplateObjectDescriptor::create(decoder.vm(), TemplateObjectDescriptor::create(WTF::move(decodedRawStrings), WTF::move(decodedCookedStrings)), m_endOffset);
    }

private:
    friend struct CoreCodecSelfTestAccess;

    CachedVector<CachedString, 4> m_rawStrings;
    CachedVector<CachedOptional<CachedString>, 4> m_cookedStrings;
    int m_endOffset;
};

class CachedBigInt : public VariableLengthObject<JSBigInt> {
public:
    void encode(Encoder& encoder, JSBigInt& bigInt)
    {
        m_length = bigInt.length();
        m_sign = bigInt.sign();

        if (!m_length)
            return;

        unsigned size = sizeof(JSBigInt::Digit) * m_length;
        uint8_t* buffer = this->allocate(encoder, size, alignof(JSBigInt::Digit));
        memcpy(buffer, bigInt.dataStorage(), size);
    }

    JSBigInt* decode(Decoder& decoder) const
    {
        if (!m_length)
            return decoder.vm().heapBigIntConstantZero.get();

        // JSBigInt::tryCreateWithLength needs a length of at most maxLength, and the digits lie inside the payload
        // (SPEC-ucb.codec.md, E15).
        if (decoder.validates() && !coreCodecCheck(decoder, m_length <= JSBigInt::maxLength && this->placedTarget<JSBigInt::Digit>(decoder, m_length)))
            return nullptr;
        JSBigInt* bigInt = JSBigInt::tryCreateWithLength(decoder.vm(), m_length);
        RELEASE_ASSERT(bigInt);
        bigInt->setSign(m_sign);
        if (m_length)
            memcpy(bigInt->dataStorage(), this->buffer(), sizeof(JSBigInt::Digit) * m_length);
        return bigInt;
    }

private:
    friend struct CoreCodecSelfTestAccess;

    static constexpr unsigned maximumLength() { return JSBigInt::maxLength; }

    unsigned m_length;
    bool m_sign;
    uint8_t m_unused[3] { };
};

// A constant is a kind byte and a 4-byte slot; the owner keeps the kinds in a parallel array (CachedJSValuePool). Small
// values live in the slot itself. Everything else is what VariableLengthObject already does: an inline/external string,
// or the slot-relative offset of a record.
class CachedJSValue : public VariableLengthObject<WriteBarrier<Unknown>> {
public:
    enum class Kind : uint8_t {
        Undefined,
        Null,
        True,
        False,
        Empty,
        Int32, // the slot holds the value
        Double, // the slot points at the 8 raw bytes
        SymbolTable,
        String,
        ImmutableButterfly,
        RegExp,
        TemplateObjectDescriptor,
        BigInt,
        // JITCache: the VM's ordered-hash-table sentinel, which builtins compare by identity; no payload
        // (SPEC-ucb.codec.md, E16).
        OrderedHashTableSentinel,
    };

    Kind encode(Encoder& encoder, JSValue v)
    {
        if (v.isEmpty())
            return Kind::Empty;
        if (!v.isCell()) {
            if (v.isInt32()) {
                this->setRawSlot(static_cast<uint32_t>(v.asInt32()));
                return Kind::Int32;
            }
            if (v.isUndefined())
                return Kind::Undefined;
            if (v.isNull())
                return Kind::Null;
            if (v.isTrue())
                return Kind::True;
            if (v.isFalse())
                return Kind::False;
            RELEASE_ASSERT(v.isDouble());
            *this->allocate<EncodedJSValue>(encoder) = JSValue::encode(v);
            return Kind::Double;
        }

        JSCell* cell = v.asCell();

        if (auto* symbolTable = dynamicDowncast<SymbolTable>(cell)) {
            this->allocate<CachedSymbolTable>(encoder)->encode(encoder, *symbolTable);
            return Kind::SymbolTable;
        }

        if (auto* string = dynamicDowncast<JSString>(cell)) {
            auto str = string->tryGetValue();
            RELEASE_ASSERT(str.data.impl()); // constants are never unresolved ropes; a failed resolution must not be encoded as garbage
            StringImpl& impl = *str.data.impl();
            if (!encoder.externalStrings() && this->tryEncodeInlineString(impl))
                return Kind::String;
            if (this->tryEncodeExternalString(encoder, impl))
                return Kind::String;
            if (auto existing = encoder.cachedOffsetForStringContents(impl)) {
                this->pointAtPayloadOffset(encoder, *existing);
                return Kind::String;
            }
            auto* record = this->allocateFor<CachedUniquedStringImpl>(encoder, impl);
            record->encode(encoder, impl);
            encoder.cacheStringContents(impl, encoder.offsetOf(record));
            return Kind::String;
        }

        // JITCache: a core writes the sentinel by identity, before the butterfly it also is (SPEC-ucb.codec.md, E16).
        if (encoder.isCore() && cell == encoder.vm().orderedHashTableSentinel())
            return Kind::OrderedHashTableSentinel;

        if (auto* immutableButterfly = dynamicDowncast<JSCellButterfly>(cell)) {
            this->allocate<CachedImmutableButterfly>(encoder)->encode(encoder, *immutableButterfly);
            return Kind::ImmutableButterfly;
        }

        if (auto* regexp = dynamicDowncast<RegExp>(cell)) {
            this->allocate<CachedRegExp>(encoder)->encode(encoder, *regexp);
            return Kind::RegExp;
        }

        if (auto* templateObjectDescriptor = dynamicDowncast<JSTemplateObjectDescriptor>(cell)) {
            this->allocate<CachedTemplateObjectDescriptor>(encoder)->encode(encoder, *templateObjectDescriptor);
            return Kind::TemplateObjectDescriptor;
        }

        if (auto* bigInt = dynamicDowncast<JSBigInt>(cell)) {
            this->allocate<CachedBigInt>(encoder)->encode(encoder, *bigInt);
            return Kind::BigInt;
        }

        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue decode(Decoder& decoder, Kind kind, CachedJSValuePoolOwner poolOwner) const
    {
        // A failed check of the validating decode leaves undefined (SPEC-ucb.codec.md, E15).
        if (decoder.validates() && !coreCodecCheck(decoder, isWellFormedKind(kind, poolOwner)))
            return jsUndefined();
        switch (kind) {
        case Kind::Undefined:
            return jsUndefined();
        case Kind::Null:
            return jsNull();
        case Kind::True:
            return jsBoolean(true);
        case Kind::False:
            return jsBoolean(false);
        case Kind::Empty:
            return JSValue();
        case Kind::Int32:
            return jsNumber(static_cast<int32_t>(this->rawSlot()));
        case Kind::Double: {
            const EncodedJSValue* bits = checkedRecord<EncodedJSValue>(decoder);
            if (!bits)
                return jsUndefined();
            if (decoder.validates() && !coreCodecCheck(decoder, isWellFormedDouble(*bits)))
                return jsUndefined();
            return JSValue::decode(*bits);
        }
        case Kind::SymbolTable:
            if (auto* symbolTable = checkedRecord<CachedSymbolTable>(decoder))
                return symbolTable->decode(decoder);
            return jsUndefined();
        case Kind::String: {
            if (this->hasInlineString()) {
                if (decoder.validates() && !coreCodecCheck(decoder, this->hasWellFormedInlineString()))
                    return jsUndefined();
                return jsOwnedString(decoder.vm(), String { this->inlineString(decoder) });
            }
            if (this->hasExternalString()) {
                // A JITCacheCore encoder never writes an external-string ordinal.
                if (decoder.validates() && !coreCodecCheck(decoder, false))
                    return jsUndefined();
                return decoder.jsStringForExternalString(this->externalStringOrdinal());
            }
            auto* string = checkedRecord<CachedUniquedStringImpl>(decoder);
            if (!string)
                return jsUndefined();
            // A JSString's value is never a symbol, so the encoder never writes one here (SPEC-ucb.codec.md, E15).
            if (decoder.validates() && !coreCodecCheck(decoder, !string->isSymbol()))
                return jsUndefined();
            // JITCache: a core's own string constant registers decode as the atoms generation makes them, and jsString gives
            // the VM's own empty and one-character strings (SPEC-ucb.codec.md, E10). Constants inside cells keep their
            // native decode.
            if (decoder.purpose() == Decoder::Purpose::JITCacheCore && poolOwner == CachedJSValuePoolOwner::CodeBlock)
                return jsString(decoder.vm(), String { adoptRef(*static_cast<StringImpl*>(string->decode(decoder))) });
            // A constant becomes a JSString; it does not have to be an atom, so skip the atom table.
            return jsString(decoder.vm(), string->decodePlainString(decoder));
        }
        case Kind::ImmutableButterfly:
            return decodeCell<CachedImmutableButterfly>(decoder);
        case Kind::RegExp:
            return decodeCell<CachedRegExp>(decoder);
        case Kind::TemplateObjectDescriptor:
            return decodeCell<CachedTemplateObjectDescriptor>(decoder);
        case Kind::BigInt:
            return decodeCell<CachedBigInt>(decoder);
        case Kind::OrderedHashTableSentinel:
            // A generated or imported UCB holds its own VM's sentinel (SPEC-ucb.codec.md, E16).
            return decoder.vm().orderedHashTableSentinel();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    // The record this slot names, which the validating decode checks first; null when a check fails.
    template<typename T>
    const T* checkedRecord(Decoder& decoder) const
    {
        if (!decoder.validates())
            return this->template buffer<T>();
        const T* record = this->template placedTarget<T>(decoder);
        if (!coreCodecCheck(decoder, record && coreCodecIsWellFormed(decoder, *record)))
            return nullptr;
        return record;
    }

    template<typename T>
    JSValue decodeCell(Decoder& decoder) const
    {
        if (auto* record = checkedRecord<T>(decoder)) {
            if (auto* cell = record->decode(decoder))
                return cell;
        }
        return jsUndefined();
    }

    // A kind the encoder writes. A butterfly holds the values of an array literal's constant elements, which
    // ArrayNode::emitBytecode takes from ConstantNodes: null, booleans, numbers, strings and BigInts. It never holds a
    // cell that holds constants itself, so a pool never nests in another.
    static bool isWellFormedKind(Kind kind, CachedJSValuePoolOwner poolOwner)
    {
        if (kind > Kind::OrderedHashTableSentinel)
            return false;
        if (poolOwner == CachedJSValuePoolOwner::CodeBlock)
            return true;
        switch (kind) {
        case Kind::Null:
        case Kind::True:
        case Kind::False:
        case Kind::Int32:
        case Kind::Double:
        case Kind::String:
        case Kind::BigInt:
            return true;
        default:
            return false;
        }
    }

    // A Double record must decode to a double: other bits could name a cell, which the collector would follow. Its negation
    // must be no impure NaN either, since the baseline's negate fast path flips bit 63 of the boxed value and so turns such a
    // double into bits that name a cell, whether code reads it from a constant register or from a butterfly element. Every
    // NaN generation writes passes: a constant register holds jsNaN(), and an element holds the NaN the parser folded, with
    // the CPU's bits and possibly its sign flipped (SPEC-ucb.codec.md, E15).
    static bool isWellFormedDouble(EncodedJSValue bits)
    {
        JSValue value = JSValue::decode(bits);
        return value.isDouble() && !isImpureNaN(-value.asDouble());
    }
};
static_assert(sizeof(CachedJSValue) == sizeof(uint32_t));

// `count` kind bytes, then (4-aligned) `count` slots.
struct CachedJSValuePool {
    static size_t byteSize(unsigned count) { return roundUpToMultipleOf<4>(static_cast<size_t>(count)) + sizeof(CachedJSValue) * count; }
    static const CachedJSValue* slots(const uint8_t* pool, unsigned count) { return reinterpret_cast<const CachedJSValue*>(pool + roundUpToMultipleOf<4>(static_cast<size_t>(count))); }

    // Returns the pool's payload offset.
    static ptrdiff_t encode(Encoder& encoder, std::span<const WriteBarrier<Unknown>> values)
    {
        unsigned count = values.size();
        ASSERT(count);
        auto result = encoder.malloc(byteSize(count), alignof(CachedJSValue));
        uint8_t* kinds = result.buffer();
        CachedJSValue* slots = new (kinds + roundUpToMultipleOf<4>(static_cast<size_t>(count))) CachedJSValue[count];
        for (unsigned i = 0; i < count; ++i)
            kinds[i] = static_cast<uint8_t>(slots[i].encode(encoder, values[i].get()));
        return result.offset();
    }

    static void decode(Decoder& decoder, const uint8_t* pool, unsigned count, WriteBarrier<Unknown>* out, const JSCell* owner, CachedJSValuePoolOwner poolOwner)
    {
        const CachedJSValue* slot = slots(pool, count);
        for (unsigned i = 0; i < count; ++i)
            out[i].set(decoder.vm(), owner, slot[i].decode(decoder, static_cast<CachedJSValue::Kind>(pool[i]), poolOwner));
    }
};

inline void CachedJSValuePoolRef::encode(Encoder& encoder, std::span<const WriteBarrier<Unknown>> values)
{
    if (values.empty())
        return;
    this->pointAtPayloadOffset(encoder, CachedJSValuePool::encode(encoder, values));
}

inline bool CachedJSValuePoolRef::isPlaced(Decoder& decoder, unsigned count) const
{
    return this->placedBytes(decoder, CachedJSValuePool::byteSize(count), alignof(CachedJSValue));
}

inline void CachedJSValuePoolRef::decode(Decoder& decoder, WriteBarrier<Unknown>* out, unsigned count, const JSCell* owner) const
{
    if (!count)
        return;
    // The only pool reached through a slot is an immutable butterfly's, which CachedImmutableButterfly::decode placed.
    ASSERT(!decoder.validates() || isPlaced(decoder, count));
    CachedJSValuePool::decode(decoder, this->buffer(), count, out, owner, CachedJSValuePoolOwner::Butterfly);
}

// The elements lie inside the payload and hold only what ArrayNode::emitBytecode stores under the indexing type, which the
// tiers read without checking (SPEC-ucb.codec.md, E15). An Int32 butterfly holds only int32 values, which the DFG reads as
// boxed int32s. A Double butterfly holds no NaN: indexingTypeForValue gives a NaN element ContiguousShape, double storage
// reads any NaN as a hole, and JSCellButterfly::get asserts against one and boxes each element with jsDoubleNumber, which
// turns an impure NaN into bits that are no double and can name a cell.
inline bool CachedImmutableButterfly::hasWellFormedElements(Decoder& decoder) const
{
    if (!m_length)
        return true;
    if (hasDouble(m_indexingType)) {
        const double* elements = m_cachedDoubles.placedTarget<double>(decoder, m_length);
        return elements && std::ranges::none_of(std::span { elements, m_length }, [](double element) {
            return std::isnan(element);
        });
    }
    if (!m_cachedValues.isPlaced(decoder, m_length))
        return false;
    if (m_indexingType != CopyOnWriteArrayWithInt32)
        return true;
    return std::ranges::all_of(m_cachedValues.kinds(m_length), [](uint8_t kind) {
        return kind == static_cast<uint8_t>(CachedJSValue::Kind::Int32);
    });
}

inline JSCellButterfly* CachedImmutableButterfly::decode(Decoder& decoder) const
{
    if (decoder.validates()) {
        // VM::cellButterflyStructure indexes its table with the indexing type, which must be one of the three copy-on-write
        // array types (SPEC-ucb.codec.md, E15).
        bool isCopyOnWriteArray = m_indexingType == CopyOnWriteArrayWithInt32 || m_indexingType == CopyOnWriteArrayWithDouble || m_indexingType == CopyOnWriteArrayWithContiguous;
        if (!coreCodecCheck(decoder, isCopyOnWriteArray && m_length <= IndexingHeader::maximumLength && hasWellFormedElements(decoder)))
            return nullptr;
    }
    JSCellButterfly* immutableButterfly = JSCellButterfly::create(decoder.vm(), m_indexingType, m_length);
    if (hasDouble(m_indexingType))
        m_cachedDoubles.decode(decoder, immutableButterfly->toButterfly()->contiguousDouble().data(), m_length, immutableButterfly);
    else
        m_cachedValues.decode(decoder, immutableButterfly->toButterfly()->contiguous().data(), m_length, immutableButterfly);
    return immutableButterfly;
}

// UnlinkedMetadataTable's offset table is cumulative and most opcodes have no metadata in a given function, so a code
// block stores only the opcodes that have entries: (opcode << 24 | entry count). A typical function has a handful instead
// of 51. Counts rather than offsets because sizeof(Op::Metadata) is the decoder's, not the encoder's (Bun cross-compiles
// executables that embed the payload).
struct CachedMetadataSteps {
    static constexpr unsigned indexShift = UnlinkedMetadataTable::stepIndexShift;
    static constexpr uint32_t countMask = UnlinkedMetadataTable::stepCountMask;
    static_assert(UnlinkedMetadataTable::s_offsetTableEntries < (1u << (32 - indexShift)));

    // The inverse of UnlinkedMetadataTable::finalize(): (opcode << 24 | entry count) back out of the offset table.
    static Vector<uint32_t, 16> compute(const UnlinkedMetadataTable& metadataTable)
    {
        ASSERT(metadataTable.m_isFinalized && metadataTable.m_hasMetadata);
        Vector<uint32_t, 16> steps;
        if (metadataTable.m_isBackedBySteps && !metadataTable.m_isLinked) {
            steps.append(std::span { metadataTable.m_steps, metadataTable.m_stepsCount });
            return steps;
        }
        auto offsetAt = [&](unsigned i) -> uint32_t { return metadataTable.m_is32Bit ? metadataTable.offsetTable32()[i] : metadataTable.offsetTable16()[i]; };
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries - 1; ++i) {
            auto opcode = static_cast<OpcodeID>(i);
            uint32_t start = roundUpToMultipleOf(metadataAlignment(opcode), offsetAt(i));
            uint32_t end = offsetAt(i + 1);
            if (end <= start)
                continue;
            uint32_t count = (end - start) / metadataSize(opcode);
            ASSERT(count && start + count * metadataSize(opcode) == end);
            RELEASE_ASSERT(count <= countMask); // or it would spill into the opcode bits
            steps.append(i << indexShift | count);
        }
#if ASSERT_ENABLED
        std::array<UnlinkedMetadataTable::Offset32, UnlinkedMetadataTable::s_offsetTableEntries> check;
        UnlinkedMetadataTable::expandSteps(steps.span(), check.data());
        for (unsigned i = 0; i < UnlinkedMetadataTable::s_offsetTableEntries; ++i)
            ASSERT(check[i] == offsetAt(i));
#endif
        return steps;
    }

    // JITCache: what UnlinkedMetadataTable::expandSteps asserts: strictly increasing opcodes below s_offsetTableEntries - 1
    // that lay out a table whose offsets fit 32 bits (SPEC-ucb.codec.md, E15).
    static bool stepsAreWellFormed(std::span<const uint32_t> steps)
    {
        uint64_t offset = UnlinkedMetadataTable::s_offset16TableSize;
        unsigned nextOpcode = 0;
        for (uint32_t step : steps) {
            unsigned opcode = step >> indexShift;
            if (opcode < nextOpcode || opcode >= UnlinkedMetadataTable::s_offsetTableEntries - 1)
                return false;
            nextOpcode = opcode + 1;
            offset = roundUpToMultipleOf(metadataAlignment(static_cast<OpcodeID>(opcode)), offset);
            offset += static_cast<uint64_t>(step & countMask) * metadataSize(static_cast<OpcodeID>(opcode));
            if (offset + UnlinkedMetadataTable::s_offset32TableSize > std::numeric_limits<UnlinkedMetadataTable::Offset32>::max())
                return false;
        }
        return true;
    }

    static Ref<UnlinkedMetadataTable> build(unsigned numValueProfiles, std::span<const uint32_t> steps)
    {
        Ref<UnlinkedMetadataTable> metadataTable = UnlinkedMetadataTable::create(UnlinkedMetadataTable::stepsNeed32BitOffsets(steps), numValueProfiles);
        metadataTable->m_isFinalized = true;
        metadataTable->m_isLinked = false;
        metadataTable->m_hasMetadata = true;
        metadataTable->m_numValueProfiles = numValueProfiles;
        if (metadataTable->m_is32Bit)
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable32());
        else
            UnlinkedMetadataTable::expandSteps(steps, metadataTable->offsetTable16());
        return metadataTable;
    }
};

// Arrays a code block refers to from its varint tail by (count, offset) instead of through an 8-byte CachedVector member.
// Plain arrays may be shared with an identical one written earlier (see Encoder::ShareableArrayScope).
template<typename T, typename Container>
static ptrdiff_t encodeArrayForTail(Encoder& encoder, const Container& container)
{
    unsigned size = container.size();
    ASSERT(size);
    if constexpr (std::is_same_v<T, SourceType<T>> && std::is_trivially_copyable_v<T>) {
        // A container, or a span (an instruction stream's bytes, which an owned and a borrowed stream both have).
        const T* elements;
        if constexpr (requires { container.span(); })
            elements = container.span().data();
        else
            elements = container.data();
        auto bytes = std::span { std::bit_cast<const uint8_t*>(elements), sizeof(T) * size };
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(bytes) ^ static_cast<unsigned>(bytes.size());
        if (encoder.arraySharingEnabled()) {
            if (auto existing = encoder.existingIdenticalArray(bytes, hash, alignof(T))) {
                encoder.noteSharedArray(*existing, bytes.size());
                return *existing;
            }
        }
        auto result = encoder.malloc(bytes.size(), alignof(T));
        memcpySpan(std::span { result.buffer(), bytes.size() }, bytes);
        encoder.registerArray(hash, result.offset(), bytes.size());
        return result.offset();
    } else {
        static_assert(PayloadType<T>);
        auto result = encoder.malloc(sizeof(T) * size, alignof(T));
        T* buffer = new (result.buffer()) T[size];
        for (unsigned i = 0; i < size; ++i)
            ::JSC::encode(encoder, buffer[i], container[i]);
        return result.offset();
    }
}

template<typename T, typename Container, typename... Args>
static void decodeArrayFromTail(Decoder& decoder, const void* elements, unsigned size, Container& out, Args... args)
{
    if (!size)
        return;
    out = Container(size);
    const T* buffer = static_cast<const T*>(elements);
    for (unsigned i = 0; i < size; ++i)
        ::JSC::decode(decoder, buffer[i], out[i], args...);
}

class CachedSourceOrigin : public CachedObject<SourceOrigin> {
public:
    void encode(Encoder& encoder, const SourceOrigin& sourceOrigin)
    {
        m_string.encode(encoder, sourceOrigin.url().string());
    }

    SourceOrigin decode(Decoder& decoder) const
    {
        return SourceOrigin { URL({ }, m_string.decode(decoder)) };
    }

private:
    CachedString m_string;
};

class CachedTextPosition : public CachedObject<TextPosition> {
public:
    void encode(Encoder&, TextPosition textPosition)
    {
        m_line = textPosition.m_line.zeroBasedInt();
        m_column = textPosition.m_column.zeroBasedInt();
    }

    TextPosition decode(Decoder&) const
    {
        return TextPosition { OrdinalNumber::fromZeroBasedInt(m_line), OrdinalNumber::fromZeroBasedInt(m_column) };
    }

private:
    int m_line;
    int m_column;
};

template <typename Source, typename CachedType>
class CachedSourceProviderShape : public CachedObject<Source> {
public:
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
    {
        m_sourceOrigin.encode(encoder, sourceProvider.sourceOrigin());
        m_sourceURL.encode(encoder, sourceProvider.sourceURL());
        m_preRedirectURL.encode(encoder, sourceProvider.preRedirectURL());
        m_sourceURLDirective.encode(encoder, sourceProvider.sourceURLDirective());
        m_sourceMappingURLDirective.encode(encoder, sourceProvider.sourceMappingURLDirective());
        m_startPosition.encode(encoder, sourceProvider.startPosition());
        m_sourceTaintedOrigin = sourceProvider.sourceTaintedOrigin();
    }

    void decode(Decoder& decoder, SourceProvider& sourceProvider) const
    {
        sourceProvider.setSourceURLDirective(m_sourceURLDirective.decode(decoder));
        sourceProvider.setSourceMappingURLDirective(m_sourceMappingURLDirective.decode(decoder));
        sourceProvider.setSourceTaintedOrigin(m_sourceTaintedOrigin);
    }

protected:
    CachedSourceOrigin m_sourceOrigin;
    CachedString m_sourceURL;
    CachedString m_preRedirectURL;
    CachedString m_sourceURLDirective;
    CachedString m_sourceMappingURLDirective;
    CachedTextPosition m_startPosition;
    SourceTaintedOrigin m_sourceTaintedOrigin;
    uint8_t m_unused[3] { };
};

class CachedStringSourceProvider : public CachedSourceProviderShape<StringSourceProvider, CachedStringSourceProvider> {
    using Base = CachedSourceProviderShape<StringSourceProvider, CachedStringSourceProvider>;

public:
#if USE(BUN_JSC_ADDITIONS)
    // Takes the base type for the same reason decode() returns it: Bun's runtime
    // provider is a SourceProvider sibling of StringSourceProvider, and only
    // base-class API is used below.
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
#else
    void encode(Encoder& encoder, const StringSourceProvider& sourceProvider)
#endif
    {
        Base::encode(encoder, sourceProvider);
#if USE(BUN_JSC_ADDITIONS)
        // SourceCodeKey::operator== under BUN_JSC_ADDITIONS does not compare source
        // text, so encoding it here only wastes ~source_size bytes of bytecode and
        // forces a ~source_size heap allocation at decode time. Store length only —
        // the comparison still validates length() and host().
        m_sourceLength = sourceProvider.source().length();
#else
        m_source.encode(encoder, sourceProvider.source().toString());
#endif
    }

#if USE(BUN_JSC_ADDITIONS)
    // The caller (CachedSourceProvider::decode) returns SourceProvider*, so the
    // BUN reuse path can return the runtime provider as its base type without
    // any reinterpret_cast through the StringSourceProvider sibling.
    SourceProvider* decode(Decoder& decoder, SourceProviderSourceType sourceType) const
#else
    StringSourceProvider* decode(Decoder& decoder, SourceProviderSourceType sourceType) const
#endif
    {
#if USE(BUN_JSC_ADDITIONS)
        // Reuse the runtime SourceProvider the Decoder was constructed with rather
        // than allocating a fresh StringSourceProvider holding a heap copy of the
        // source. The decoded key is only used for SourceCodeKey equality, which
        // under BUN_JSC_ADDITIONS does not look at source bytes.
        //
        // Base::decode is intentionally skipped: the runtime provider already has
        // its sourceURLDirective / sourceMappingURLDirective / sourceTaintedOrigin
        // set, and the decoded key only needs sourceOrigin().url().host() and
        // length() for equality. CachedSourceProviderShape fields are offset-based
        // (not stream-based), so leaving them undecoded does not affect later reads.
        if (RefPtr<SourceProvider> provider = decoder.provider()) {
            if (provider->sourceType() == sourceType && provider->source().length() == m_sourceLength)
                return provider.leakRef();
        }
        // Fallback for callers that did not supply a provider: decode without source
        // bytes. SourceCodeKey::operator== ignores string(), but length() is compared,
        // so synthesize a provider whose source() is empty — length() will mismatch
        // and the cache entry will be rejected, which is the conservative behaviour.
        String decodedSource;
#else
        String decodedSource = m_source.decode(decoder);
#endif
        SourceOrigin decodedSourceOrigin = m_sourceOrigin.decode(decoder);
        String decodedSourceURL = m_sourceURL.decode(decoder);
        TextPosition decodedStartPosition = m_startPosition.decode(decoder);

        Ref<StringSourceProvider> sourceProvider = StringSourceProvider::create(decodedSource, decodedSourceOrigin, decodedSourceURL, m_sourceTaintedOrigin, decodedStartPosition, sourceType);
        Base::decode(decoder, sourceProvider.get());
        return &sourceProvider.leakRef();
    }

private:
#if USE(BUN_JSC_ADDITIONS)
    unsigned m_sourceLength;
#else
    CachedString m_source;
#endif
};

#if ENABLE(WEBASSEMBLY)
class CachedWebAssemblySourceProvider : public CachedSourceProviderShape<WebAssemblySourceProvider, CachedWebAssemblySourceProvider> {
    using Base = CachedSourceProviderShape<WebAssemblySourceProvider, CachedWebAssemblySourceProvider>;

public:
    void encode(Encoder& encoder, const WebAssemblySourceProvider& sourceProvider)
    {
        Base::encode(encoder, sourceProvider);
        m_data.encode(encoder, sourceProvider.dataVector());
    }

    WebAssemblySourceProvider* decode(Decoder& decoder) const
    {
        Vector<uint8_t> decodedData;
        SourceOrigin decodedSourceOrigin = m_sourceOrigin.decode(decoder);
        String decodedSourceURL = m_sourceURL.decode(decoder);

        m_data.decode(decoder, decodedData);

        Ref<WebAssemblySourceProvider> sourceProvider = WebAssemblySourceProvider::create(WTF::move(decodedData), decodedSourceOrigin, decodedSourceURL);
        Base::decode(decoder, sourceProvider.get());

        return &sourceProvider.leakRef();
    }

private:
    CachedVector<uint8_t> m_data;
};
#endif

class CachedSourceProvider : public VariableLengthObject<SourceProvider> {
public:
    void encode(Encoder& encoder, const SourceProvider& sourceProvider)
    {
        m_sourceType = sourceProvider.sourceType();
        switch (m_sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
            this->allocate<CachedStringSourceProvider>(encoder)->encode(encoder, sourceProvider);
#else
            this->allocate<CachedStringSourceProvider>(encoder)->encode(encoder, reinterpret_cast<const StringSourceProvider&>(sourceProvider));
#endif
            break;
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly:
            this->allocate<CachedWebAssemblySourceProvider>(encoder)->encode(encoder, reinterpret_cast<const WebAssemblySourceProvider&>(sourceProvider));
            break;
#endif
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    SourceProvider* decode(Decoder& decoder) const
    {
        switch (m_sourceType) {
        case SourceProviderSourceType::Program:
        case SourceProviderSourceType::Module:
#if USE(BUN_JSC_ADDITIONS)
        case SourceProviderSourceType::BunTranspiledModule:
#endif
            return this->buffer<CachedStringSourceProvider>()->decode(decoder, m_sourceType);
#if ENABLE(WEBASSEMBLY)
        case SourceProviderSourceType::WebAssembly:
            return this->buffer<CachedWebAssemblySourceProvider>()->decode(decoder);
#endif
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

private:
    SourceProviderSourceType m_sourceType;
    uint8_t m_unused[3] { };
};

template<typename Source>
class CachedUnlinkedSourceCodeShape : public CachedObject<Source> {
public:
    void encode(Encoder& encoder, const UnlinkedSourceCode& sourceCode)
    {
        m_provider.encode(encoder, sourceCode.m_provider);
        m_startOffset = sourceCode.startOffset();
        m_endOffset = sourceCode.endOffset();
    }

    void decode(Decoder& decoder, UnlinkedSourceCode& sourceCode) const
    {
        sourceCode.m_provider = m_provider.decode(decoder);
        sourceCode.m_startOffset = m_startOffset;
        sourceCode.m_endOffset = m_endOffset;
    }

private:
    CachedRefPtr<CachedSourceProvider> m_provider;
    int m_startOffset;
    int m_endOffset;
};


class CachedUnlinkedSourceCode : public CachedUnlinkedSourceCodeShape<UnlinkedSourceCode> { };

class CachedSourceCode : public CachedUnlinkedSourceCodeShape<SourceCode> {
    using Base = CachedUnlinkedSourceCodeShape<SourceCode>;

public:
    void encode(Encoder& encoder, const SourceCode& sourceCode)
    {
        Base::encode(encoder, sourceCode);
        m_firstLine = sourceCode.firstLine().zeroBasedInt();
        m_startColumn = sourceCode.startColumn().zeroBasedInt();
    }

    void decode(Decoder& decoder, SourceCode& sourceCode) const
    {
        Base::decode(decoder, sourceCode);
        sourceCode.m_firstLine = OrdinalNumber::fromZeroBasedInt(m_firstLine);
        sourceCode.m_startColumn = OrdinalNumber::fromZeroBasedInt(m_startColumn);
    }

private:
    int m_firstLine;
    int m_startColumn;
};

class CachedTDZEnvironmentLink : public CachedObject<TDZEnvironmentLink> {
public:
    void encode(Encoder& encoder, const TDZEnvironmentLink& environment)
    {
        // JITCache: the encoder's start chain is written as the start record, an empty handle and an empty parent, which no
        // native link has, since each holds an environment from CompactTDZEnvironmentMap::get (SPEC-ucb.codec.md, E14).
        if (&environment == encoder.startChain())
            return;
        m_handle.encode(encoder, environment.m_handle);
        encodeChain(encoder, m_parent, environment.m_parent.get());
    }

    // JITCache: writes a TDZ chain, a UFE's or a link's parent. A chain equal to the encoder's start chain, as many links
    // and, link by link, the same environment object, is written as the start chain's own record; any other chain link by
    // link until what remains equals it. The VM interns environments by content, so comparing environments rather than
    // links cuts a natively decoded UCB's chains, whose links are fresh, where a generated UCB's are cut (E14).
    static void encodeChain(Encoder& encoder, CachedRefPtr<CachedTDZEnvironmentLink>& slot, const TDZEnvironmentLink* chain)
    {
        const TDZEnvironmentLink* startChain = encoder.startChain();
        if (startChain && chain && equalChains(chain, startChain))
            chain = startChain;
        slot.encode(encoder, chain);
    }

    TDZEnvironmentLink* decode(Decoder& decoder) const
    {
        if (decoder.purpose() == Decoder::Purpose::JITCacheCore) {
            if (m_handle.isEmpty())
                return decodeStartRecord(decoder);
            if (decoder.validates())
                return decodeValidatedChain(decoder);
        }
        CompactTDZEnvironmentMap::Handle handle = m_handle.decode(decoder);
        RefPtr<TDZEnvironmentLink> parent = m_parent.decode(decoder);
        return new TDZEnvironmentLink(WTF::move(handle), WTF::move(parent));
    }

    // JITCache: the TDZ chain digest of SPEC-ucb.md section 3.4. A link's digest hashes its environment's digest and its
    // parent's digest, 32 zero bytes after the last link, so the links are listed and digested from the chain's end. A
    // chain longer than the links the list keeps inline moves the list to the heap, which the budget is charged for before
    // the list is allocated and while it lives (SPEC-ucb.codec.md, section 2).
    static std::optional<std::array<uint8_t, 32>> chainDigest(const TDZEnvironmentLink* chain, CoreEncodingBudget* budget, unsigned& environmentsDigested)
    {
        std::array<uint8_t, 32> digest { };
        if (!chain)
            return digest;
        constexpr size_t inlineLinks = 32;
        size_t count = 0;
        for (const TDZEnvironmentLink* link = chain; link; link = link->m_parent.get())
            ++count;
        size_t linksBytes = count > inlineLinks ? count * sizeof(const TDZEnvironmentLink*) : 0;
        if (budget && linksBytes && !budget->charge(linksBytes))
            return std::nullopt;
        auto releaseLinks = makeScopeExit([&] {
            if (budget && linksBytes)
                budget->release(linksBytes);
        });
        Vector<const TDZEnvironmentLink*, inlineLinks> links;
        links.reserveInitialCapacity(count);
        ASSERT(encoderHeapBytes(links) == linksBytes);
        for (const TDZEnvironmentLink* link = chain; link; link = link->m_parent.get())
            links.append(link);
        for (size_t i = links.size(); i--;) {
            // Every live link holds an environment the VM interned.
            const CompactTDZEnvironment* environment = CachedCompactTDZEnvironmentMapHandle::environmentOf(links[i]->m_handle);
            RELEASE_ASSERT(environment);
            std::optional<std::array<uint8_t, 32>> environmentDigest = CachedCompactTDZEnvironment::contentDigest(*environment, budget, environmentsDigested);
            if (!environmentDigest)
                return std::nullopt;
            JITCache::SHA256 hasher;
            hasher.update("JITCache.tdzl.v1"_s.span8());
            hasher.update(*environmentDigest);
            hasher.update(digest);
            digest = hasher.finalize();
        }
        return digest;
    }

private:
    friend struct CoreCodecSelfTestAccess;

    static bool equalChains(const TDZEnvironmentLink* chain, const TDZEnvironmentLink* other)
    {
        // At most one pair of links per link of the shorter chain.
        while (chain && other) {
            if (chain == other)
                return true;
            if (CachedCompactTDZEnvironmentMapHandle::environmentOf(chain->m_handle) != CachedCompactTDZEnvironmentMapHandle::environmentOf(other->m_handle))
                return false;
            chain = chain->m_parent.get();
            other = other->m_parent.get();
        }
        return !chain && !other;
    }

    // The start record stands for the decoder's start chain and has an empty parent (E14). The decoder's finalizer releases
    // one reference to the chain, as it releases one for each link the decode allocates.
    TDZEnvironmentLink* decodeStartRecord(Decoder& decoder) const
    {
        TDZEnvironmentLink* startChain = decoder.startChain();
        if (!startChain || !m_parent.ptr().isEmpty()) {
            decoder.noteFailure(CoreDecodeFailure::Malformed);
            return nullptr;
        }
        startChain->ref();
        return startChain;
    }

    // The validating decode walks the chain instead of recursing into each parent, so that a chain as long as the payload
    // allows decodes without exhausting the stack (E15). It follows parents, each placed and marked in the record cache, until
    // an empty one, a start record or a link the decoder already decoded. A parent whose decode has not returned, this record
    // included, which CachedPtr::decodeValidated marked, closes a loop, and one of another type fails as well. The walk then
    // decodes from the deepest record up, recording each ancestor's link and handing its allocation's reference to the
    // decoder's finalizer, as CachedRefPtr::decode does; CachedPtr::decodeValidated records this record's link itself.
    TDZEnvironmentLink* decodeValidatedChain(Decoder& decoder) const
    {
        Vector<const CachedTDZEnvironmentLink*, 16> records;
        RefPtr<TDZEnvironmentLink> parent;
        for (const CachedTDZEnvironmentLink* record = this;;) {
            records.append(record);
            if (record->m_handle.isEmpty() || record->m_parent.ptr().isEmpty())
                break;
            const CachedTDZEnvironmentLink* next = record->m_parent.ptr().placedTarget<CachedTDZEnvironmentLink>(decoder);
            if (!coreCodecCheck(decoder, !!next))
                return nullptr;
            void* decoded = nullptr;
            CoreCodecRecordState state = coreCodecRecordState<CachedTDZEnvironmentLink>(decoder, next, decoded);
            if (state == CoreCodecRecordState::Decoded) {
                // A link this decoder decoded before, or null when it failed, which already recorded the failure.
                parent = static_cast<TDZEnvironmentLink*>(decoded);
                if (!parent)
                    return nullptr;
                break;
            }
            if (!coreCodecCheck(decoder, state == CoreCodecRecordState::Unvisited))
                return nullptr;
            coreCodecMarkRecord<CachedTDZEnvironmentLink>(decoder, next);
            record = next;
        }
        // When a link fails, the failure is recorded and the records above it stay marked without a link, so that a later
        // slot naming one of them fails too.
        for (size_t i = records.size(); i--;) {
            const CachedTDZEnvironmentLink* record = records[i];
            TDZEnvironmentLink* link;
            if (record->m_handle.isEmpty()) {
                link = record->decodeStartRecord(decoder);
                if (!link)
                    return nullptr;
            } else {
                CompactTDZEnvironmentMap::Handle handle = record->m_handle.decode(decoder);
                // An environment that failed decodes as an empty handle, after recording the failure.
                if (!handle)
                    return nullptr;
                link = new TDZEnvironmentLink(WTF::move(handle), parent);
            }
            if (!i)
                return link;
            coreCodecSetRecordObject(decoder, record, link);
            decoder.addFinalizer([link] {
                link->deref();
            });
            parent = link;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }

    CachedCompactTDZEnvironmentMapHandle m_handle;
    CachedRefPtr<CachedTDZEnvironmentLink> m_parent;
};

class CachedJSTextPosition : public CachedObject<JSTextPosition> {
public:
    void encode(Encoder&, const JSTextPosition& position)
    {
        m_line = position.line;
        m_offset = position.offset;
        m_lineStartOffset = position.lineStartOffset;
    }

    JSTextPosition decode(Decoder&) const
    {
        return JSTextPosition { m_line, m_offset, m_lineStartOffset };
    }

private:
    int m_line;
    int m_offset;
    int m_lineStartOffset;
};

class CachedClassElementDefinition : public CachedObject<UnlinkedFunctionExecutable::ClassElementDefinition> {
public:
    void encode(Encoder& encoder, const UnlinkedFunctionExecutable::ClassElementDefinition& definition)
    {
        m_ident.encode(encoder, definition.ident);
        m_position.encode(encoder, definition.position);
        m_initializerPosition.encode(encoder, definition.initializerPosition);
        m_kind = static_cast<uint8_t>(definition.kind);
    }

    void decode(Decoder& decoder, UnlinkedFunctionExecutable::ClassElementDefinition& definition) const
    {
        definition.ident = m_ident.decode(decoder);
        definition.position = m_position.decode(decoder);
        definition.initializerPosition = m_initializerPosition.decode(decoder);
        // The kind is cast to its enum (SPEC-ucb.codec.md, E15); a malformed one keeps the default.
        using Kind = UnlinkedFunctionExecutable::ClassElementDefinition::Kind;
        if (decoder.validates() && !coreCodecCheck(decoder, m_kind <= static_cast<uint8_t>(Kind::StaticInitializationBlock)))
            return;
        definition.kind = static_cast<Kind>(m_kind);
    }

private:
    CachedIdentifier m_ident;
    CachedJSTextPosition m_position;
    CachedOptional<CachedJSTextPosition> m_initializerPosition;
    uint8_t m_kind;
    uint8_t m_unused[3] { };
};

// A header word of presence bits, then only the members that are set: the three vectors (4-byte aligned), then the
// class source's four numbers as varints. Most rare data is an async function's (empty) wrapper parameter names.
class CachedFunctionExecutableRareData : public CachedObject<UnlinkedFunctionExecutable::RareData> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif
    enum Header : uint32_t {
        HasClassSource = 1 << 0,
        HasWrapperParameterNames = 1 << 1,
        HasClassElementDefinitions = 1 << 2,
        HasParentPrivateNameEnvironment = 1 << 3,
    };

    static uint32_t headerFor(const UnlinkedFunctionExecutable::RareData& rareData)
    {
        uint32_t header = 0;
        if (!rareData.m_classSource.isNull())
            header |= HasClassSource;
        if (!rareData.m_generatorOrAsyncWrapperFunctionParameterNames.isEmpty())
            header |= HasWrapperParameterNames;
        if (!rareData.m_classElementDefinitions.isEmpty())
            header |= HasClassElementDefinitions;
        if (!rareData.m_parentPrivateNameEnvironment.isEmpty())
            header |= HasParentPrivateNameEnvironment;
        return header;
    }

    static size_t tailSize(const UnlinkedFunctionExecutable::RareData& rareData)
    {
        uint32_t header = headerFor(rareData);
        size_t size = 0;
        if (header & HasWrapperParameterNames)
            size += sizeof(CachedVector<CachedIdentifier>);
        if (header & HasClassElementDefinitions)
            size += sizeof(CachedVector<CachedClassElementDefinition>);
        if (header & HasParentPrivateNameEnvironment)
            size += sizeof(CachedPrivateNameEnvironment);
        if (header & HasClassSource)
            size += packClassSource(rareData.m_classSource).size();
        return size;
    }

    void encode(Encoder& encoder, const UnlinkedFunctionExecutable::RareData& rareData)
    {
        m_header = headerFor(rareData);
        uint8_t* p = std::bit_cast<uint8_t*>(this) + sizeof(uint32_t);
        if (m_header & HasWrapperParameterNames) {
            auto* names = new (p) CachedVector<CachedIdentifier>();
            p += sizeof(*names);
            names->encode(encoder, rareData.m_generatorOrAsyncWrapperFunctionParameterNames);
        }
        if (m_header & HasClassElementDefinitions) {
            auto* definitions = new (p) CachedVector<CachedClassElementDefinition>();
            p += sizeof(*definitions);
            definitions->encode(encoder, rareData.m_classElementDefinitions);
        }
        if (m_header & HasParentPrivateNameEnvironment) {
            auto* environment = new (p) CachedPrivateNameEnvironment();
            p += sizeof(*environment);
            environment->encodeShared(encoder, rareData.m_parentPrivateNameEnvironment);
        }
        if (m_header & HasClassSource) {
            VarintWriter writer = packClassSource(rareData.m_classSource);
            writer.copyTo(p);
        }
    }

    // JITCache: the checks of the validating decode, on a record whose header word lies inside the payload: only the
    // members the encoder writes, each inside the payload, and the class source's varints ending there (SPEC-ucb.codec.md, E15).
    bool isWellFormed(Decoder& decoder) const
    {
        if (m_header & ~(HasClassSource | HasWrapperParameterNames | HasClassElementDefinitions | HasParentPrivateNameEnvironment))
            return false;
        size_t members = 0;
        if (m_header & HasWrapperParameterNames)
            members += sizeof(CachedVector<CachedIdentifier>);
        if (m_header & HasClassElementDefinitions)
            members += sizeof(CachedVector<CachedClassElementDefinition>);
        if (m_header & HasParentPrivateNameEnvironment)
            members += sizeof(CachedPrivateNameEnvironment);
        const uint8_t* p = std::bit_cast<const uint8_t*>(this) + sizeof(uint32_t);
        if (!coreCodecHolds(decoder, p, members, alignof(uint32_t)))
            return false;
        if (!(m_header & HasClassSource))
            return true;
        auto payload = decoder.payloadSpan();
        VarintReader reader(p + members, payload.data() + payload.size());
        for (unsigned i = 0; i < 4; ++i)
            reader.u32();
        return !reader.overran();
    }

    UnlinkedFunctionExecutable::RareData* decode(Decoder& decoder) const
    {
        UnlinkedFunctionExecutable::RareData* rareData = new UnlinkedFunctionExecutable::RareData { };
        const uint8_t* p = std::bit_cast<const uint8_t*>(this) + sizeof(uint32_t);
        if (m_header & HasWrapperParameterNames) {
            reinterpret_cast<const CachedVector<CachedIdentifier>*>(p)->decode(decoder, rareData->m_generatorOrAsyncWrapperFunctionParameterNames);
            p += sizeof(CachedVector<CachedIdentifier>);
        }
        if (m_header & HasClassElementDefinitions) {
            reinterpret_cast<const CachedVector<CachedClassElementDefinition>*>(p)->decode(decoder, rareData->m_classElementDefinitions);
            p += sizeof(CachedVector<CachedClassElementDefinition>);
        }
        if (m_header & HasParentPrivateNameEnvironment) {
            reinterpret_cast<const CachedPrivateNameEnvironment*>(p)->decode(decoder, rareData->m_parentPrivateNameEnvironment);
            p += sizeof(CachedPrivateNameEnvironment);
        }
        if (m_header & HasClassSource) {
            VarintReader reader(p);
            SourceCode& source = rareData->m_classSource;
            source.m_provider = decoder.provider();
            source.m_startOffset = reader.u32();
            source.m_endOffset = source.m_startOffset + reader.u32();
            source.m_firstLine = OrdinalNumber::fromZeroBasedInt(reader.i32());
            source.m_startColumn = OrdinalNumber::fromZeroBasedInt(reader.i32());
        }
        return rareData;
    }

private:
    static VarintWriter packClassSource(const SourceCode& source)
    {
        VarintWriter writer;
        writer.u32(source.startOffset());
        writer.u32(source.endOffset() - source.startOffset());
        writer.i32(source.firstLine().zeroBasedInt());
        writer.i32(source.startColumn().zeroBasedInt());
        return writer;
    }

    uint32_t m_header;
};
static_assert(sizeof(CachedFunctionExecutableRareData) == sizeof(uint32_t));

// Layout: a header word (what is present, parse mode), then only what is present, 4-byte slots first so they stay aligned:
//   [mutable metadata 8][checksum 4][extent 4]   updatable records (the jsc shell's disk cache patches them in place)
//   [checksum 4][extent 4]                       checksummed but not updatable
//   [call slot][construct slot][name][TDZ link][rare data]
//   varint tail: flags, features, lexically scoped features, source positions, parameter count, [line info]
// A persistent payload (bun --compile) has none of the first two rows and only the slots it uses.
class CachedFunctionExecutable : public CachedObject<UnlinkedFunctionExecutable> {
    friend struct CachedFunctionExecutableOffsets;

public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif

    enum Header : uint32_t {
        HasCallSlot = 1 << 0,
        HasConstructSlot = 1 << 1,
        HasName = 1 << 2,
        HasTDZ = 1 << 3,
        HasRareData = 1 << 4,
        HasLines = 1 << 5,
        HasChecksum = 1 << 6,
        Updatable = 1 << 7, // implies HasChecksum and both code block slots
        HasCapturedVariables = 1 << 8,
        ParseModeShift = 16, // 8 bits
    };

    struct Scalars {
        unsigned firstLineOffset;
        unsigned lineCount;
        unsigned unlinkedFunctionStart;
        unsigned unlinkedBodyStartColumn;
        unsigned unlinkedBodyEndColumn;
        unsigned startOffset;
        unsigned sourceLength;
        unsigned parametersStartOffset;
        unsigned unlinkedFunctionEnd;
        unsigned parameterCount;
        SourceParseMode sourceParseMode;
        ImplementationVisibility implementationVisibility;
        bool isBuiltinFunction;
        bool isBuiltinDefaultClassConstructor;
        unsigned constructAbility;
        unsigned constructorKind;
        unsigned functionMode;
        unsigned scriptMode;
        unsigned superBinding;
        unsigned derivedContextType;
        unsigned evalContextType;
        bool inlineAttribute;
        bool needsClassFieldInitializer;
        unsigned privateBrandRequirement;
        bool hasName;
        CodeFeatures features;
        LexicallyScopedFeatures lexicallyScopedFeatures;
        bool hasCapturedVariables;
    };

    using CodeBlockSlot = CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock>;

    // The record, parsed.
    struct View {
        uint32_t header { 0 };
        const CachedFunctionExecutableMetadata* metadata { nullptr };
        const uint32_t* checksum { nullptr };
        const uint32_t* extent { nullptr };
        const CodeBlockSlot* call { nullptr };
        const CodeBlockSlot* construct { nullptr };
        const CachedIdentifier* name { nullptr };
        const CachedRefPtr<CachedTDZEnvironmentLink>* tdz { nullptr };
        const CachedPtr<CachedFunctionExecutableRareData>* rareData { nullptr };
        const uint8_t* tail { nullptr };
        const uint8_t* tailEnd { nullptr };
        Scalars scalars;
        uint32_t flags { 0 }; // the packed fields, as written
        bool intact { false };
    };

    static size_t tailSize(const Encoder& encoder, const UnlinkedFunctionExecutable& executable)
    {
        // Everything after the header word; see encode() for the order.
        return slotBytes(headerFor(executable, &encoder)) + packedTail(executable, encoder.purpose()).size();
    }

    void encode(Encoder&, const UnlinkedFunctionExecutable&);
    UnlinkedFunctionExecutable* decode(Decoder&) const;

    // `limit` bounds the parse; the payload end for an integrity check, unbounded once verified.
    View view(const uint8_t* limit = nullptr) const;

    // Checked by the owning code block (or the cache entry) before it decodes anything.
    bool isIntact(Decoder& decoder) const
    {
        if (!decoder.payloadContains(this, sizeof(uint32_t)))
            return false;
        auto payload = decoder.payloadSpan();
        View v = view(payload.data() + payload.size());
        if (!v.intact)
            return false;
        if (decoder.validates() && !hasCoreTags(decoder, v))
            return false;
        if (!(v.header & HasChecksum))
            return true;
        return decoder.regionChecksumMatches(this, *v.extent, v.checksum);
    }

    // JITCache: the validating decode reaches a child's record through its slot (SPEC-ucb.codec.md, E15).
    bool isWellFormed(Decoder& decoder) const { return isIntact(decoder); }

private:
    friend struct CoreCodecSelfTestAccess;

    // JITCache: the header and tags a JITCacheCore encoder writes (SPEC-ucb.codec.md, E15).
    bool hasCoreTags(Decoder&, const View&) const;

    static uint32_t headerFor(const UnlinkedFunctionExecutable&, const Encoder*);
    static size_t slotBytes(uint32_t header)
    {
        size_t bytes = 0;
        if (header & Updatable)
            bytes += sizeof(CachedFunctionExecutableMetadata);
        if (header & HasChecksum)
            bytes += 2 * sizeof(uint32_t);
        for (uint32_t bit : { HasCallSlot, HasConstructSlot, HasName, HasTDZ, HasRareData }) {
            if (header & bit)
                bytes += sizeof(uint32_t);
        }
        return bytes;
    }
    static Vector<uint8_t, 64> packedTail(const UnlinkedFunctionExecutable&, Encoder::Purpose);
    static void packScalars(const UnlinkedFunctionExecutable&, Encoder::Purpose, VarintWriter&);

    uint8_t* bytes() { return std::bit_cast<uint8_t*>(this); }
    const uint8_t* bytes() const { return std::bit_cast<const uint8_t*>(this); }

    uint32_t m_header;
};
static_assert(sizeof(CachedFunctionExecutable) == sizeof(uint32_t));

// Fixed offsets of an updatable record (see CachedFunctionExecutable::Header).
ptrdiff_t CachedFunctionExecutableOffsets::metadataOffset()
{
    return sizeof(uint32_t);
}

ptrdiff_t CachedFunctionExecutableOffsets::checksumOffset()
{
    return metadataOffset() + sizeof(CachedFunctionExecutableMetadata);
}

ptrdiff_t CachedFunctionExecutableOffsets::extentOffset()
{
    return checksumOffset() + sizeof(uint32_t);
}

ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForCallOffset()
{
    return extentOffset() + sizeof(uint32_t);
}

ptrdiff_t CachedFunctionExecutableOffsets::codeBlockForConstructOffset()
{
    return codeBlockForCallOffset() + sizeof(uint32_t);
}

size_t CachedFunctionExecutableOffsets::fixedSize()
{
    return codeBlockForConstructOffset() + sizeof(uint32_t);
}

bool CachedFunctionExecutableOffsets::isUpdatable(std::span<const uint8_t> record)
{
    uint32_t header;
    if (record.size() < sizeof(header))
        return false;
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(&header), sizeof(header) }, record.first(sizeof(header)));
    return header & CachedFunctionExecutable::Updatable;
}

uint32_t bytecodeCacheRecordChecksum(std::span<const uint8_t> record, size_t checksumOffset)
{
    static const uint8_t zeros[4] = { };
    uint32_t crc = ~0u;
    crc = crc32c(crc, record.first(checksumOffset));
    crc = crc32c(crc, std::span { zeros, 4 });
    crc = crc32c(crc, record.subspan(checksumOffset + 4));
    return ~crc;
}

template<typename CodeBlockType> struct CachedCodeBlockRecordFor;

class CachedProgramCodeBlock;
class CachedModuleCodeBlock;
class CachedEvalCodeBlock;
class CachedFunctionCodeBlock;
template<> struct CachedCodeBlockRecordFor<UnlinkedProgramCodeBlock> { using type = CachedProgramCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedModuleProgramCodeBlock> { using type = CachedModuleCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedEvalCodeBlock> { using type = CachedEvalCodeBlock; };
template<> struct CachedCodeBlockRecordFor<UnlinkedFunctionCodeBlock> { using type = CachedFunctionCodeBlock; };

// The few members most code blocks never have; written (before the record, like everything else) only when one is set.
struct CachedCodeBlockExtras {
    void encode(Encoder& encoder, const UnlinkedCodeBlock& codeBlock)
    {
        rareData.encode(encoder, codeBlock.m_rareData.get());
        outOfLineJumpTargets.encode(encoder, codeBlock.m_outOfLineJumpTargets);
    }
    static bool isNeeded(const UnlinkedCodeBlock& codeBlock)
    {
        return codeBlock.m_rareData || !codeBlock.m_outOfLineJumpTargets.isEmpty();
    }

    CachedPtr<CachedCodeBlockRareData> rareData;
    CachedHashMap<JSInstructionStream::Offset, int> outOfLineJumpTargets;
};

// JITCache: an instruction stream the validating decode accepts (SPEC-ucb.codec.md, E15). Walked from offset 0, each
// instruction starts with an opcode below NUMBER_OF_BYTECODE_IDS, after at most one op_wide16 or op_wide32 prefix, and is
// not itself a prefix; its whole BaseInstruction::size() lies inside the stream; and the walk ends exactly at the end.
static bool coreCodecIsWholeInstructionStream(std::span<const uint8_t> bytes)
{
    static_assert(maxJSOpcodeIDWidth == OpcodeSize::Narrow, "every width writes the opcode in one byte");
    size_t offset = 0;
    while (offset < bytes.size()) {
        size_t remaining = bytes.size() - offset;
        bool isPrefixed = bytes[offset] == op_wide16 || bytes[offset] == op_wide32;
        size_t opcodeAt = isPrefixed ? 1 : 0;
        if (opcodeAt >= remaining)
            return false;
        uint8_t opcode = bytes[offset + opcodeAt];
        if (opcode >= NUMBER_OF_BYTECODE_IDS || opcode == op_wide16 || opcode == op_wide32)
            return false;
        size_t size = std::bit_cast<const JSInstruction*>(bytes.data() + offset)->size();
        if (size > remaining)
            return false;
        offset += size;
    }
    return true;
}

// A code block is written as one region: its arrays (metadata steps, instructions, constants, identifiers, child slots,
// extras), then a 16-byte record followed by a varint tail that says where in the region each array is and holds every
// count/register/flag, then whatever the derived record adds, then the children's executable records.
// Offsets in the tail are relative to the start of the region, so they are 1-2 bytes for nearly every function.
template<typename CodeBlockType>
class CachedCodeBlock : public CachedObject<CodeBlockType> {
public:
#if USE(BUN_JSC_ADDITIONS)
    static constexpr bool isSingleOwner = true;
#endif
    using Record = typename CachedCodeBlockRecordFor<CodeBlockType>::type;

    struct Scalars {
        VirtualRegister thisRegister;
        VirtualRegister scopeRegister;
        unsigned isConstructor : 1;
        unsigned isBuiltinDefaultClassConstructor : 1;
        unsigned isBuiltinFunction : 1;
        unsigned superBinding : 1;
        unsigned scriptMode : 1;
        unsigned isArrowFunctionContext : 1;
        unsigned isClassContext : 1;
        unsigned constructorKind : 2;
        unsigned derivedContextType : 2;
        unsigned evalContextType : 2;
        unsigned hasTailCalls : 1;
        unsigned codeType : 2;
        unsigned hasCheckpoints : 1;
        SourceParseMode parseMode;
        OptionSet<CodeGenerationMode> codeGenerationMode;
        int numVars;
        int numCalleeLocals;
        int numParameters;
        unsigned numValueProfiles;
        unsigned numArrayProfiles;
        unsigned numBinaryArithProfiles;
        unsigned numUnaryArithProfiles;
    };

    enum LayoutFlag : uint8_t {
        LayoutHasMetadata = 1 << 0,
        LayoutHasExtras = 1 << 2,
        LayoutHasChecksum = 1 << 3, // [u32 region size][u32 checksum] follow the tail
    };
    struct Array {
        unsigned count { 0 };
        int32_t at { 0 }; // relative to the region start; only meaningful when count is non-zero
    };
    struct Layout {
        uint8_t flags { 0 };
        unsigned recordOffsetInRegion { 0 };
        unsigned metadataValueProfiles { 0 };
        Array steps;
        Array instructions; // count is in bytes
        Array constants;
        Array constantsSourceCodeRepresentation;
        Array identifiers;
        Array functionDecls;
        Array functionExprs;
        int32_t extrasAt { 0 };
    };
    struct Tail {
        Layout layout;
        Scalars scalars;
        uint32_t flags { 0 }; // the packed scalar flags, as written
        const uint8_t* end { nullptr }; // where the tail's varints stop: the region size and checksum, when present, follow
        bool intact { true };
    };

    static Record* create(Encoder&, const CodeBlockType&);
    void decode(Decoder&, UnlinkedCodeBlock&, const Tail&) const;

    // `limit` bounds the parse for the integrity check; once the region is verified it is read unbounded.
    Tail readTail(const uint8_t* limit = nullptr) const;
    // The tail the decode in progress already parsed (see ActiveTailScope), else a fresh parse.
    const Tail& tail(Decoder& decoder, Tail& storage) const
    {
        if (auto* active = static_cast<const Tail*>(decoder.activeCodeBlockTail(this)))
            return *active;
        storage = readTail();
        return storage;
    }
    struct ActiveTailScope {
        ActiveTailScope(Decoder& decoder, const void* record, const Tail& tail)
            : m_decoder(decoder)
        {
            decoder.setActiveCodeBlockTail(record, &tail);
        }
        ~ActiveTailScope() { m_decoder.setActiveCodeBlockTail(nullptr, nullptr); }
        Decoder& m_decoder;
    };
    Scalars scalars(Decoder& decoder) const { Tail storage; return tail(decoder, storage).scalars; }

    const uint8_t* regionBegin(const Layout& layout) const { return std::bit_cast<const uint8_t*>(this) - layout.recordOffsetInRegion; }
    template<typename T> const T* at(const Layout& layout, const Array& array) const { return array.count ? reinterpret_cast<const T*>(regionBegin(layout) + array.at) : nullptr; }
    const CachedCodeBlockExtras* extras(const Layout& layout) const { return layout.flags & LayoutHasExtras ? reinterpret_cast<const CachedCodeBlockExtras*>(regionBegin(layout) + layout.extrasAt) : nullptr; }

    JSInstructionStream* instructions(Decoder& decoder) const
    {
        Tail storage;
        const Layout& layout = tail(decoder, storage).layout;
        std::span<const uint8_t> bytes { at<uint8_t>(layout, layout.instructions), layout.instructions.count };
        // JITCache: the validating decode hands back a stream made of whole instructions, or an empty one after recording
        // the failure, so that every later walk over the stream stays inside it (SPEC-ucb.codec.md, E15).
        if (decoder.validates() && !coreCodecCheck(decoder, coreCodecIsWholeInstructionStream(bytes)))
            bytes = { };
        if (decoder.canBorrowPayload())
            return new JSInstructionStream(bytes, JSInstructionStream::Borrow);
        Vector<uint8_t, 0, UnsafeVectorOverflow, 16, InstructionStreamBufferMalloc> copy;
        copy.append(bytes);
        return new JSInstructionStream(WTF::move(copy));
    }

    Ref<UnlinkedMetadataTable> metadata(Decoder& decoder) const
    {
        Tail storage;
        const Layout& layout = tail(decoder, storage).layout;
        if (!(layout.flags & LayoutHasMetadata))
            return UnlinkedMetadataTable::empty();
        std::span<const uint32_t> steps { at<uint32_t>(layout, layout.steps), layout.steps.count };
        // JITCache: UnlinkedMetadataTable::expandSteps asserts what the validating decode checks first (SPEC-ucb.codec.md, E15).
        if (decoder.validates() && !coreCodecCheck(decoder, CachedMetadataSteps::stepsAreWellFormed(steps)))
            return UnlinkedMetadataTable::empty();
        if (decoder.canBorrowPayload())
            return UnlinkedMetadataTable::createFromPersistentSteps(layout.metadataValueProfiles, steps);
        return CachedMetadataSteps::build(layout.metadataValueProfiles, steps);
    }

    UnlinkedCodeBlock::RareData* rareData(Decoder& decoder) const
    {
        Tail storage;
        auto* e = extras(tail(decoder, storage).layout);
        return e ? e->rareData.decode(decoder) : nullptr;
    }

    // The region (arrays, record, tail, derived members, child slots) is checksummed when the payload carries checksums; a
    // mismatch on decode means the payload is damaged and the block is generated from source instead. Without them the
    // check is that everything the block points at lies inside the payload.
    bool regionIsIntact(Decoder& decoder, Tail& tail) const
    {
        // `this` came from a slot that CachedBytecode::commitUpdates may rewrite and is therefore not itself checksummed.
        if (!decoder.payloadContains(this, sizeof(Record)))
            return false;
        auto payload = decoder.payloadSpan();
        const uint8_t* payloadEnd = payload.data() + payload.size();
        tail = readTail(payloadEnd);
        if (!tail.intact)
            return false;
        const Layout& layout = tail.layout;
        const uint8_t* begin = regionBegin(layout);
        if (begin > std::bit_cast<const uint8_t*>(this) || begin < payload.data())
            return false;
        const uint8_t* end = payloadEnd;
        uint32_t regionSize = 0;
        const uint8_t* storedChecksum = nullptr;
        if (layout.flags & LayoutHasChecksum) {
            if (!decoder.payloadContains(tail.end, 2 * sizeof(uint32_t)))
                return false;
            memcpy(&regionSize, tail.end, sizeof(regionSize));
            storedChecksum = tail.end + sizeof(uint32_t);
            end = begin + regionSize;
            if (!decoder.payloadContains(begin, regionSize) || storedChecksum + sizeof(uint32_t) > end)
                return false;
        }

        // Every array must lie inside the region, except the three the encoder may have shared from an earlier block,
        // which are folded into the checksum instead (in encoder order).
        std::array<std::span<const uint8_t>, 3> external;
        unsigned externalCount = 0;
        auto coveredBytes = [&](const Array& array, size_t bytes) {
            if (!array.count)
                return true;
            const uint8_t* p = begin + array.at;
            return array.at >= 0 && p + bytes <= end && p + bytes >= p;
        };
        auto covered = [&](const Array& array, size_t elementSize, bool shareable) {
            if (!array.count)
                return true;
            size_t bytes = elementSize * array.count;
            const uint8_t* p = begin + array.at;
            if (array.at >= 0 && p + bytes <= end && p + bytes >= p)
                return true;
            if (!shareable || !decoder.payloadContains(p, bytes))
                return false;
            external[externalCount++] = { p, bytes };
            return true;
        };
        if (!covered(layout.steps, sizeof(uint32_t), true)
            || !covered(layout.instructions, 1, true)
            || !covered(layout.constantsSourceCodeRepresentation, sizeof(SourceCodeRepresentation), true)
            || !coveredBytes(layout.constants, CachedJSValuePool::byteSize(layout.constants.count))
            || !covered(layout.identifiers, sizeof(CachedIdentifier), false)
            || !covered(layout.functionDecls, sizeof(CachedWriteBarrier<CachedFunctionExecutable>), false)
            || !covered(layout.functionExprs, sizeof(CachedWriteBarrier<CachedFunctionExecutable>), false))
            return false;
        if ((layout.flags & LayoutHasExtras) && (layout.extrasAt < 0 || begin + layout.extrasAt + sizeof(CachedCodeBlockExtras) > end))
            return false;
        if (decoder.validates() && !hasCoreLayout(decoder, tail))
            return false;
        if (storedChecksum && !decoder.regionChecksumMatches(begin, regionSize, reinterpret_cast<const uint32_t*>(storedChecksum), std::span { external.data(), externalCount }))
            return false;

        for (const Array* children : { &layout.functionDecls, &layout.functionExprs }) {
            auto* slots = at<CachedWriteBarrier<CachedFunctionExecutable>>(layout, *children);
            for (unsigned i = 0; i < children->count; ++i) {
                // JITCache: the validating decode also places the child's record at its alignment (SPEC-ucb.codec.md, E15).
                const CachedFunctionExecutable* record = decoder.validates() ? slots[i].ptr().template placedTarget<CachedFunctionExecutable>(decoder) : slots[i].ptr().getIfInPayload(decoder);
                if (!record || !record->isIntact(decoder))
                    return false;
            }
        }
        return true;
    }

protected:
    // Derived records with nothing of their own use these.
    void encodeOwnMembers(Encoder&, const CodeBlockType&) { }
    void decodeOwnMembers(Decoder&, CodeBlockType&) const { }

private:
    friend struct CoreCodecSelfTestAccess;

    // JITCache: what the validating decode adds to the region's checks (SPEC-ucb.codec.md, E15).
    bool hasCoreLayout(Decoder&, const Tail&) const;

    static void packScalars(const UnlinkedCodeBlock&, VarintWriter&);
    static void packLayout(const Layout&, VarintWriter&);
    const uint8_t* tailBytes() const { return std::bit_cast<const uint8_t*>(this) + sizeof(Record); }
    uint8_t* tailBytes() { return std::bit_cast<uint8_t*>(this) + sizeof(Record); }

    CachedPtr<CachedExpressionInfo> m_expressionInfo; // written by the deferred cold pass, so it stays a fixed slot
};

// The members only Program/Eval/Module code has (UnlinkedGlobalCodeBlock); a function record does not pay for them.
template<typename CodeBlockType>
class CachedGlobalCodeBlock : public CachedCodeBlock<CodeBlockType> {
    using Base = CachedCodeBlock<CodeBlockType>;

protected:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedGlobalCodeBlock& codeBlock)
    {
        m_features = codeBlock.m_features;
        m_lexicallyScopedFeatures = codeBlock.m_lexicallyScopedFeatures;
        m_hasCapturedVariables = codeBlock.m_hasCapturedVariables;
        m_lineCount = codeBlock.m_lineCount;
        m_endColumn = codeBlock.m_endColumn;
        m_sourceURLDirective.encode(encoder, codeBlock.m_sourceURLDirective.get());
        m_sourceMappingURLDirective.encode(encoder, codeBlock.m_sourceMappingURLDirective.get());
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedGlobalCodeBlock& codeBlock) const
    {
        codeBlock.m_features = m_features;
        codeBlock.m_lexicallyScopedFeatures = m_lexicallyScopedFeatures;
        codeBlock.m_hasCapturedVariables = m_hasCapturedVariables;
        codeBlock.m_lineCount = m_lineCount;
        codeBlock.m_endColumn = m_endColumn;
        codeBlock.m_sourceURLDirective = m_sourceURLDirective.decode(decoder);
        codeBlock.m_sourceMappingURLDirective = m_sourceMappingURLDirective.decode(decoder);
    }

private:
    CodeFeatures m_features;
    LexicallyScopedFeatures m_lexicallyScopedFeatures;
    bool m_hasCapturedVariables;
    unsigned m_lineCount;
    unsigned m_endColumn;
    CachedRefPtr<CachedStringImpl> m_sourceURLDirective;
    CachedRefPtr<CachedStringImpl> m_sourceMappingURLDirective;
};

class CachedProgramCodeBlock : public CachedGlobalCodeBlock<UnlinkedProgramCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedProgramCodeBlock>;
    friend CachedCodeBlock<UnlinkedProgramCodeBlock>;
    friend struct CoreCodecSelfTestAccess;

public:
    UnlinkedProgramCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedProgramCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(encoder, codeBlock);
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_lexicalDeclarations.encode(encoder, codeBlock.m_lexicalDeclarations);
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedProgramCodeBlock& codeBlock) const
    {
        Base::decodeOwnMembers(decoder, codeBlock);
        m_varDeclarations.decode(decoder, codeBlock.m_varDeclarations);
        m_lexicalDeclarations.decode(decoder, codeBlock.m_lexicalDeclarations);
    }

    CachedVariableEnvironment m_varDeclarations;
    CachedVariableEnvironment m_lexicalDeclarations;
};

class CachedModuleCodeBlock : public CachedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock>;
    friend CachedCodeBlock<UnlinkedModuleProgramCodeBlock>;

public:
    UnlinkedModuleProgramCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedModuleProgramCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(encoder, codeBlock);
        m_varDeclarations.encode(encoder, codeBlock.m_varDeclarations);
        m_moduleEnvironmentSymbolTableConstantRegisterOffset = codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset;
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedModuleProgramCodeBlock& codeBlock) const
    {
        Base::decodeOwnMembers(decoder, codeBlock);
        m_varDeclarations.decode(decoder, codeBlock.m_varDeclarations);
        codeBlock.m_moduleEnvironmentSymbolTableConstantRegisterOffset = m_moduleEnvironmentSymbolTableConstantRegisterOffset;
    }

    CachedVariableEnvironment m_varDeclarations;
    int m_moduleEnvironmentSymbolTableConstantRegisterOffset;
};

class CachedEvalCodeBlock : public CachedGlobalCodeBlock<UnlinkedEvalCodeBlock> {
    using Base = CachedGlobalCodeBlock<UnlinkedEvalCodeBlock>;
    friend CachedCodeBlock<UnlinkedEvalCodeBlock>;

public:
    UnlinkedEvalCodeBlock* decode(Decoder&) const;

private:
    void encodeOwnMembers(Encoder& encoder, const UnlinkedEvalCodeBlock& codeBlock)
    {
        Base::encodeOwnMembers(encoder, codeBlock);
        m_variables.encode(encoder, codeBlock.m_variables);
        m_functionHoistingCandidates.encode(encoder, codeBlock.m_functionHoistingCandidates);
    }
    void decodeOwnMembers(Decoder& decoder, UnlinkedEvalCodeBlock& codeBlock) const
    {
        Base::decodeOwnMembers(decoder, codeBlock);
        m_variables.decode(decoder, codeBlock.m_variables);
        m_functionHoistingCandidates.decode(decoder, codeBlock.m_functionHoistingCandidates);
    }

    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_variables;
    CachedVector<CachedIdentifier, 0, UnsafeVectorOverflow> m_functionHoistingCandidates;
};

class CachedFunctionCodeBlock : public CachedCodeBlock<UnlinkedFunctionCodeBlock> {
    using Base = CachedCodeBlock<UnlinkedFunctionCodeBlock>;
    friend Base;

public:
    UnlinkedFunctionCodeBlock* decode(Decoder&) const;
};


ALWAYS_INLINE UnlinkedFunctionCodeBlock::UnlinkedFunctionCodeBlock(Decoder& decoder, const CachedFunctionCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedFunctionCodeBlockStructure.get(), cachedCodeBlock)
{
}

template<typename T>
struct CachedCodeBlockTypeImpl;

enum class CachedCodeBlockTag {
    CachedProgramCodeBlockTag,
    CachedModuleCodeBlockTag,
    CachedEvalCodeBlockTag,
    CachedBuiltinFunctionTag, // a root UnlinkedFunctionExecutable created by BuiltinExecutables (an embedder's JS builtins)
};

static CachedCodeBlockTag NODELETE tagFromSourceCodeType(SourceCodeType type)
{
    switch (type) {
    case SourceCodeType::ProgramType:
        return CachedCodeBlockTag::CachedProgramCodeBlockTag;
    case SourceCodeType::EvalType:
        return CachedCodeBlockTag::CachedEvalCodeBlockTag;
    case SourceCodeType::ModuleType:
        return CachedCodeBlockTag::CachedModuleCodeBlockTag;
    case SourceCodeType::FunctionType:
        break;
    }
    ASSERT_NOT_REACHED();
    return static_cast<CachedCodeBlockTag>(-1);
}

template<>
struct CachedCodeBlockTypeImpl<UnlinkedProgramCodeBlock> {
    using type = CachedProgramCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedProgramCodeBlockTag;
};

template<>
struct CachedCodeBlockTypeImpl<UnlinkedModuleProgramCodeBlock> {
    using type = CachedModuleCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedModuleCodeBlockTag;
};

template<>
struct CachedCodeBlockTypeImpl<UnlinkedEvalCodeBlock> {
    using type = CachedEvalCodeBlock;
    static constexpr CachedCodeBlockTag tag = CachedCodeBlockTag::CachedEvalCodeBlockTag;
};

template<typename T>
using CachedCodeBlockType = typename CachedCodeBlockTypeImpl<T>::type;

template<typename CodeBlockType>
ALWAYS_INLINE UnlinkedCodeBlock::UnlinkedCodeBlock(Decoder& decoder, Structure* structure, const CachedCodeBlock<CodeBlockType>& cachedCodeBlock)
    : Base(decoder.vm(), structure)
    , m_age(0)
    , m_metadata(cachedCodeBlock.metadata(decoder))
    , m_instructions(cachedCodeBlock.instructions(decoder))
    , m_rareData(cachedCodeBlock.rareData(decoder))
{
    auto scalars = cachedCodeBlock.scalars(decoder);
    m_thisRegister = scalars.thisRegister;
    m_scopeRegister = scalars.scopeRegister;
    m_numVars = scalars.numVars;
    m_numCalleeLocals = scalars.numCalleeLocals;
    m_isConstructor = scalars.isConstructor;
    m_numParameters = scalars.numParameters;
    m_isBuiltinFunction = scalars.isBuiltinFunction;
    m_isBuiltinDefaultClassConstructor = scalars.isBuiltinDefaultClassConstructor;
    m_superBinding = scalars.superBinding;
    m_scriptMode = scalars.scriptMode;
    m_isArrowFunctionContext = scalars.isArrowFunctionContext;
    m_isClassContext = scalars.isClassContext;
    m_hasTailCalls = scalars.hasTailCalls;
    m_constructorKind = scalars.constructorKind;
    m_derivedContextType = scalars.derivedContextType;
    m_evalContextType = scalars.evalContextType;
    m_codeType = scalars.codeType;
    m_hasCheckpoints = scalars.hasCheckpoints;
    m_parseMode = scalars.parseMode;
    m_codeGenerationMode = scalars.codeGenerationMode;
    m_valueProfiles = FixedVector<UnlinkedValueProfile>(scalars.numValueProfiles);
    m_arrayProfiles = FixedVector<UnlinkedArrayProfile>(scalars.numArrayProfiles);
    m_binaryArithProfiles = FixedVector<BinaryArithProfile>(scalars.numBinaryArithProfiles);
    m_unaryArithProfiles = FixedVector<UnaryArithProfile>(scalars.numUnaryArithProfiles);
    m_llintExecuteCounter.setNewThreshold(thresholdForJIT(Options::thresholdForJITAfterWarmUp()));
}

template<typename CodeBlockType>
ALWAYS_INLINE void CachedCodeBlock<CodeBlockType>::decode(Decoder& decoder, UnlinkedCodeBlock& codeBlock, const Tail& tail) const
{
    const Layout& layout = tail.layout;
    // Most identifiers and many constants become atoms; let the table grow once for this block rather than as they trickle in.
    if (unsigned expected = layout.identifiers.count + layout.constants.count; expected >= 64)
        AtomStringImpl::reserveCapacityForCurrentThread(expected);
    if (layout.constants.count) {
        codeBlock.m_constantRegisters = FixedVector<WriteBarrier<Unknown>>(layout.constants.count);
        CachedJSValuePool::decode(decoder, at<uint8_t>(layout, layout.constants), layout.constants.count, codeBlock.m_constantRegisters.mutableSpan().data(), &codeBlock, CachedJSValuePoolOwner::CodeBlock);
    }
    decodeArrayFromTail<SourceCodeRepresentation>(decoder, at<SourceCodeRepresentation>(layout, layout.constantsSourceCodeRepresentation), layout.constantsSourceCodeRepresentation.count, codeBlock.m_constantsSourceCodeRepresentation);
    if (decoder.validates()) {
        // Linking switches on each representation (SPEC-ucb.codec.md, E15).
        for (auto& representation : codeBlock.m_constantsSourceCodeRepresentation) {
            if (!coreCodecCheck(decoder, representation <= SourceCodeRepresentation::LinkTimeConstant))
                representation = SourceCodeRepresentation::Other;
        }
    }
    // hasCoreLayout has checked the slot for the validating decode.
    codeBlock.m_expressionInfo = m_expressionInfo->decode(decoder);
    if (auto* e = extras(layout))
        e->outOfLineJumpTargets.decode(decoder, codeBlock.m_outOfLineJumpTargets);
    decodeArrayFromTail<CachedIdentifier>(decoder, at<CachedIdentifier>(layout, layout.identifiers), layout.identifiers.count, codeBlock.m_identifiers);
    decodeArrayFromTail<CachedWriteBarrier<CachedFunctionExecutable>>(decoder, at<CachedWriteBarrier<CachedFunctionExecutable>>(layout, layout.functionDecls), layout.functionDecls.count, codeBlock.m_functionDecls, &codeBlock);
    decodeArrayFromTail<CachedWriteBarrier<CachedFunctionExecutable>>(decoder, at<CachedWriteBarrier<CachedFunctionExecutable>>(layout, layout.functionExprs), layout.functionExprs.count, codeBlock.m_functionExprs, &codeBlock);
}

UnlinkedProgramCodeBlock* CachedProgramCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedProgramCodeBlock>(decoder.vm())) UnlinkedProgramCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedModuleProgramCodeBlock* CachedModuleCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedModuleProgramCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedModuleProgramCodeBlock>(decoder.vm())) UnlinkedModuleProgramCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedEvalCodeBlock* CachedEvalCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedEvalCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedEvalCodeBlock>(decoder.vm())) UnlinkedEvalCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}

UnlinkedFunctionCodeBlock* CachedFunctionCodeBlock::decode(Decoder& decoder) const
{
    Tail tail;
    if (!regionIsIntact(decoder, tail))
        return nullptr;
    ActiveTailScope activeTail(decoder, this, tail);
    UnlinkedFunctionCodeBlock* codeBlock = new (NotNull, allocateCell<UnlinkedFunctionCodeBlock>(decoder.vm())) UnlinkedFunctionCodeBlock(decoder, *this);
    codeBlock->finishCreation(decoder.vm());
    Base::decode(decoder, *codeBlock, tail);
    decodeOwnMembers(decoder, *codeBlock);
    return codeBlock;
}


ALWAYS_INLINE UnlinkedProgramCodeBlock::UnlinkedProgramCodeBlock(Decoder& decoder, const CachedProgramCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedProgramCodeBlockStructure.get(), cachedCodeBlock)
{
}

ALWAYS_INLINE UnlinkedModuleProgramCodeBlock::UnlinkedModuleProgramCodeBlock(Decoder& decoder, const CachedModuleCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedModuleProgramCodeBlockStructure.get(), cachedCodeBlock)
{
}

ALWAYS_INLINE UnlinkedEvalCodeBlock::UnlinkedEvalCodeBlock(Decoder& decoder, const CachedEvalCodeBlock& cachedCodeBlock)
    : Base(decoder, decoder.vm().unlinkedEvalCodeBlockStructure.get(), cachedCodeBlock)
{
}

enum CachedFunctionExecutableFlag : uint32_t {
    // one word of 1- and 2-bit fields, written as a varint (the high bits are the rarely-set ones)
    ExecutableScriptModeShift = 0,
    ExecutableSuperBindingShift = 1,
    ExecutableConstructAbilityShift = 2,
    ExecutableHasNameShift = 3,
    ExecutableConstructorKindShift = 4, // 2 bits
    ExecutableFunctionModeShift = 6, // 2
    ExecutableImplementationVisibilityShift = 8, // 2
    ExecutableDerivedContextTypeShift = 10, // 2
    ExecutableEvalContextTypeShift = 12, // 2
    ExecutablePrivateBrandRequirementShift = 14,
    ExecutableInlineAttributeShift = 15,
    ExecutableNeedsClassFieldInitializerShift = 16,
    ExecutableIsBuiltinFunctionShift = 17,
    ExecutableIsBuiltinDefaultClassConstructorShift = 18,
};
static_assert(bitWidthOfImplementationVisibility <= 2);

void CachedFunctionExecutable::packScalars(const UnlinkedFunctionExecutable& executable, Encoder::Purpose purpose, VarintWriter& writer)
{
    uint32_t flags = static_cast<uint32_t>(executable.m_scriptMode) << ExecutableScriptModeShift
        | static_cast<uint32_t>(executable.m_superBinding) << ExecutableSuperBindingShift
        | static_cast<uint32_t>(executable.m_constructAbility) << ExecutableConstructAbilityShift
        | static_cast<uint32_t>(executable.m_hasName) << ExecutableHasNameShift
        | static_cast<uint32_t>(executable.m_constructorKind) << ExecutableConstructorKindShift
        | static_cast<uint32_t>(executable.m_functionMode) << ExecutableFunctionModeShift
        | static_cast<uint32_t>(executable.m_implementationVisibility) << ExecutableImplementationVisibilityShift
        | static_cast<uint32_t>(executable.m_derivedContextType) << ExecutableDerivedContextTypeShift
        | static_cast<uint32_t>(executable.m_evalContextType) << ExecutableEvalContextTypeShift
        | static_cast<uint32_t>(executable.m_privateBrandRequirement) << ExecutablePrivateBrandRequirementShift
        | static_cast<uint32_t>(executable.m_inlineAttribute) << ExecutableInlineAttributeShift
        | static_cast<uint32_t>(executable.m_needsClassFieldInitializer) << ExecutableNeedsClassFieldInitializerShift
        | static_cast<uint32_t>(executable.m_isBuiltinFunction) << ExecutableIsBuiltinFunctionShift
        | static_cast<uint32_t>(executable.m_isBuiltinDefaultClassConstructor) << ExecutableIsBuiltinDefaultClassConstructorShift;
    writer.u32(flags);
    // JITCache: a core writes its children unparsed, as the parent's generation creates them. The lexically scoped features
    // come from that generation, which has read the child's directive prologue (SPEC-ucb.codec.md, E2).
    writer.u32(purpose == Encoder::Purpose::JITCacheCore ? 0 : executable.m_features);
    writer.u8(static_cast<uint8_t>(executable.m_lexicallyScopedFeatures));
    // Source positions cluster around the function's start, so all but the first are deltas.
    unsigned start = executable.m_startOffset;
    writer.u32(start);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedFunctionStart - start));
    writer.i32(static_cast<int32_t>(executable.m_parametersStartOffset - start));
    writer.u32(executable.m_sourceLength);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedFunctionEnd - (start + executable.m_sourceLength)));
    // Columns are offsets from a line start; on one line the two differ by the same constant, so the second is a delta of the first.
    int32_t bodyStartColumnDelta = static_cast<int32_t>(executable.m_unlinkedBodyStartColumn - executable.m_unlinkedFunctionStart);
    writer.i32(bodyStartColumnDelta);
    writer.i32(static_cast<int32_t>(executable.m_unlinkedBodyEndColumn - executable.m_unlinkedFunctionEnd) - bodyStartColumnDelta);
    writer.u32(executable.m_parameterCount);
    if (executable.m_firstLineOffset || executable.m_lineCount) {
        writer.u32(executable.m_firstLineOffset);
        writer.u32(executable.m_lineCount);
    }
}

Vector<uint8_t, 64> CachedFunctionExecutable::packedTail(const UnlinkedFunctionExecutable& executable, Encoder::Purpose purpose)
{
    VarintWriter writer;
    packScalars(executable, purpose, writer);
    Vector<uint8_t, 64> bytes;
    bytes.grow(writer.size());
    writer.copyTo(bytes.mutableSpan().data());
    return bytes;
}

uint32_t CachedFunctionExecutable::headerFor(const UnlinkedFunctionExecutable& executable, const Encoder* encoder)
{
    uint32_t header = static_cast<uint32_t>(executable.m_sourceParseMode) << ParseModeShift;
    if (executable.m_firstLineOffset || executable.m_lineCount)
        header |= HasLines;
    if (!executable.ecmaName().isNull())
        header |= HasName;
    if (executable.m_parentScopeTDZVariables)
        header |= HasTDZ;
    if (executable.m_rareData)
        header |= HasRareData;
    // JITCache: a core writes no child body and none of the fields the child's own parse sets, and never reads the UFE's
    // slots, which share storage with a decoder while a natively decoded parent's child is lazily cached (SPEC-ucb.codec.md,
    // E1 and E2; SPEC-ucb.md F1).
    if (encoder->isCore())
        return header;
    if (executable.m_hasCapturedVariables)
        header |= HasCapturedVariables;
    if (executable.m_unlinkedCodeBlockForCall)
        header |= HasCallSlot;
    if (executable.m_unlinkedCodeBlockForConstruct)
        header |= HasConstructSlot;
    if (encoder->checksums())
        header |= HasChecksum;
    if (encoder->updatable())
        header |= Updatable | HasChecksum | HasCallSlot | HasConstructSlot;
    return header;
}

bool CachedFunctionExecutable::hasCoreTags(Decoder& decoder, const View& v) const
{
    // No code block slot, checksum or updatable field (E1 and section 4), unparsed parse fields (E2), and every packed field,
    // the parse mode and the lexically scoped features within the values their encoder writes.
    static constexpr uint32_t coreHeaderBits = HasName | HasTDZ | HasRareData | HasLines | (0xffu << ParseModeShift);
    static constexpr uint32_t knownFlags = (1u << (ExecutableIsBuiltinDefaultClassConstructorShift + 1)) - 1;
    if (!coreCodecHolds(decoder, this, sizeof(CachedFunctionExecutable), alignof(CachedFunctionExecutable)))
        return false;
    if ((v.header & ~coreHeaderBits) || (v.flags & ~knownFlags))
        return false;
    auto field = [&](unsigned shift) {
        return (v.flags >> shift) & 3;
    };
    if (field(ExecutableImplementationVisibilityShift) > static_cast<unsigned>(ImplementationVisibility::PrivateRecursive)
        || field(ExecutableDerivedContextTypeShift) > static_cast<unsigned>(DerivedContextType::DerivedMethodContext)
        || field(ExecutableEvalContextTypeShift) > static_cast<unsigned>(EvalContextType::InstanceFieldEvalContext))
        return false;
    return !v.scalars.features
        && v.scalars.lexicallyScopedFeatures <= AllLexicallyScopedFeatures
        && static_cast<unsigned>(v.scalars.sourceParseMode) <= static_cast<unsigned>(SourceParseMode::ClassStaticBlockMode);
}

auto CachedFunctionExecutable::view(const uint8_t* limit) const -> View
{
    View v;
    const uint8_t* p = bytes();
    auto room = [&](size_t n) { return !limit || (p + n <= limit && p + n >= p); };
    if (!room(sizeof(uint32_t)))
        return v;
    v.header = m_header;
    p += sizeof(uint32_t);
    auto take = [&](auto*& out) -> bool {
        using T = std::remove_const_t<std::remove_pointer_t<std::remove_reference_t<decltype(out)>>>;
        if (!room(sizeof(T)))
            return false;
        out = reinterpret_cast<const T*>(p);
        p += sizeof(T);
        return true;
    };
    if ((v.header & Updatable) && !take(v.metadata))
        return v;
    if ((v.header & HasChecksum) && (!take(v.checksum) || !take(v.extent)))
        return v;
    if ((v.header & HasCallSlot) && !take(v.call))
        return v;
    if ((v.header & HasConstructSlot) && !take(v.construct))
        return v;
    if ((v.header & HasName) && !take(v.name))
        return v;
    if ((v.header & HasTDZ) && !take(v.tdz))
        return v;
    if ((v.header & HasRareData) && !take(v.rareData))
        return v;
    v.tail = p;

    VarintReader reader(p, limit);
    Scalars& s = v.scalars;
    uint32_t flags = reader.u32();
    v.flags = flags;
    auto bits = [&](unsigned shift, unsigned width = 1) { return (flags >> shift) & ((1u << width) - 1); };
    s.scriptMode = bits(ExecutableScriptModeShift);
    s.superBinding = bits(ExecutableSuperBindingShift);
    s.constructAbility = bits(ExecutableConstructAbilityShift);
    s.hasName = bits(ExecutableHasNameShift);
    s.constructorKind = bits(ExecutableConstructorKindShift, 2);
    s.functionMode = bits(ExecutableFunctionModeShift, 2);
    s.implementationVisibility = static_cast<ImplementationVisibility>(bits(ExecutableImplementationVisibilityShift, 2));
    s.derivedContextType = bits(ExecutableDerivedContextTypeShift, 2);
    s.evalContextType = bits(ExecutableEvalContextTypeShift, 2);
    s.privateBrandRequirement = bits(ExecutablePrivateBrandRequirementShift);
    s.inlineAttribute = bits(ExecutableInlineAttributeShift);
    s.needsClassFieldInitializer = bits(ExecutableNeedsClassFieldInitializerShift);
    s.isBuiltinFunction = bits(ExecutableIsBuiltinFunctionShift);
    s.isBuiltinDefaultClassConstructor = bits(ExecutableIsBuiltinDefaultClassConstructorShift);
    s.features = static_cast<CodeFeatures>(reader.u32());
    s.lexicallyScopedFeatures = static_cast<LexicallyScopedFeatures>(reader.u8());
    s.hasCapturedVariables = v.header & HasCapturedVariables;
    s.sourceParseMode = static_cast<SourceParseMode>((v.header >> ParseModeShift) & 0xff);
    s.startOffset = reader.u32();
    s.unlinkedFunctionStart = s.startOffset + reader.i32();
    s.parametersStartOffset = s.startOffset + reader.i32();
    s.sourceLength = reader.u32();
    s.unlinkedFunctionEnd = s.startOffset + s.sourceLength + reader.i32();
    int32_t bodyStartColumnDelta = reader.i32();
    s.unlinkedBodyStartColumn = s.unlinkedFunctionStart + bodyStartColumnDelta;
    s.unlinkedBodyEndColumn = s.unlinkedFunctionEnd + bodyStartColumnDelta + reader.i32();
    s.parameterCount = reader.u32();
    if (v.header & HasLines) {
        s.firstLineOffset = reader.u32();
        s.lineCount = reader.u32();
    } else {
        s.firstLineOffset = 0;
        s.lineCount = 0;
    }
    if (v.metadata) {
        // The jsc shell's disk cache patches these after a lazily compiled function joins the cache.
        s.features = v.metadata->m_features;
        s.lexicallyScopedFeatures = v.metadata->m_lexicallyScopedFeatures;
        s.hasCapturedVariables = v.metadata->m_hasCapturedVariables;
    }
    v.tailEnd = reader.position();
    v.intact = !reader.overran();
    return v;
}

ALWAYS_INLINE void CachedFunctionExecutable::encode(Encoder& encoder, const UnlinkedFunctionExecutable& executable)
{
    uint32_t header = headerFor(executable, &encoder);
    m_header = header;
    uint8_t* p = bytes() + sizeof(uint32_t);
    CachedFunctionExecutableMetadata* metadata = nullptr;
    uint32_t* checksum = nullptr;
    uint32_t* extent = nullptr;
    CodeBlockSlot* call = nullptr;
    CodeBlockSlot* construct = nullptr;
    CachedIdentifier* name = nullptr;
    CachedRefPtr<CachedTDZEnvironmentLink>* tdz = nullptr;
    CachedPtr<CachedFunctionExecutableRareData>* rareData = nullptr;
    auto place = [&](auto*& out) {
        using T = std::remove_pointer_t<std::remove_reference_t<decltype(out)>>;
        out = new (p) T();
        p += sizeof(T);
    };
    if (header & Updatable)
        place(metadata);
    if (header & HasChecksum) {
        place(checksum);
        place(extent);
    }
    if (header & HasCallSlot)
        place(call);
    if (header & HasConstructSlot)
        place(construct);
    if (header & HasName)
        place(name);
    if (header & HasTDZ)
        place(tdz);
    if (header & HasRareData)
        place(rareData);
    {
        Vector<uint8_t, 64> tail = packedTail(executable, encoder.purpose());
        EncoderScratchCharge tailCharge(encoder, encoderHeapBytes(tail));
        memcpy(p, tail.span().data(), tail.size());
        ASSERT(p + tail.size() == bytes() + sizeof(uint32_t) + tailSize(encoder, executable));
    }
    if (metadata) {
        metadata->m_features = executable.m_features;
        metadata->m_lexicallyScopedFeatures = executable.m_lexicallyScopedFeatures;
        metadata->m_hasCapturedVariables = executable.m_hasCapturedVariables;
    }
    if (rareData)
        rareData->encode(encoder, executable.m_rareData.get());
    if (name)
        name->encode(encoder, executable.ecmaName());
    if (tdz)
        CachedTDZEnvironmentLink::encodeChain(encoder, *tdz, executable.m_parentScopeTDZVariables.get());

    // JITCache: a core neither reads the UFE's slots nor defers a body (SPEC-ucb.codec.md, E1).
    if (encoder.isCore())
        return;

    if (!executable.m_unlinkedCodeBlockForCall || !executable.m_unlinkedCodeBlockForConstruct)
        encoder.addLeafExecutable(&executable, encoder.offsetOf(this));

    if (checksum) {
        ptrdiff_t start = encoder.offsetOf(this);
        *extent = safeCast<uint32_t>(encoder.currentOffset() - start);
        encoder.addChecksum(start, *extent, encoder.offsetOf(checksum));
    }

    encoder.deferBody([call, construct, &encoder, forCall = executable.m_unlinkedCodeBlockForCall, forConstruct = executable.m_unlinkedCodeBlockForConstruct] {
        if (call)
            call->encode(encoder, forCall);
        if (construct)
            construct->encode(encoder, forConstruct);
    });
}

ALWAYS_INLINE UnlinkedFunctionExecutable* CachedFunctionExecutable::decode(Decoder& decoder) const
{
    UnlinkedFunctionExecutable* executable = new (NotNull, allocateCell<UnlinkedFunctionExecutable>(decoder.vm())) UnlinkedFunctionExecutable(decoder, *this);
    executable->finishCreation(decoder.vm());
    return executable;
}

ALWAYS_INLINE UnlinkedFunctionExecutable::UnlinkedFunctionExecutable(Decoder& decoder, const CachedFunctionExecutable& cachedExecutable)
    : Base(decoder.vm(), decoder.vm().unlinkedFunctionExecutableStructure.get())
    // JITCache: a child decoded from a core keeps weak UCB edges where a generated one would (SPEC-ucb.codec.md, E6).
    , m_isGeneratedFromCache(decoder.purpose() != Decoder::Purpose::JITCacheCore)
    , m_hasCapturedVariables(false)
    , m_isCached(false)
    , m_singletonHasBeenInvalidated(false)
    , m_features(0)
    , m_lexicallyScopedFeatures(NoLexicallyScopedFeatures)
    , m_unlinkedCodeBlockForCall()
    , m_unlinkedCodeBlockForConstruct()
{
    CachedFunctionExecutable::View v = cachedExecutable.view();
    const auto& scalars = v.scalars;
    m_hasCapturedVariables = scalars.hasCapturedVariables;
    m_features = scalars.features;
    m_lexicallyScopedFeatures = scalars.lexicallyScopedFeatures;
    if (v.name)
        m_ecmaName = v.name->decode(decoder);
    if (v.tdz)
        m_parentScopeTDZVariables = v.tdz->decode(decoder);
    if (v.rareData)
        m_rareData = std::unique_ptr<RareData>(v.rareData->decode(decoder));
    m_firstLineOffset = scalars.firstLineOffset;
    m_lineCount = scalars.lineCount;
    m_unlinkedFunctionStart = scalars.unlinkedFunctionStart;
    m_isBuiltinFunction = scalars.isBuiltinFunction;
    m_unlinkedBodyStartColumn = scalars.unlinkedBodyStartColumn;
    m_isBuiltinDefaultClassConstructor = scalars.isBuiltinDefaultClassConstructor;
    m_unlinkedBodyEndColumn = scalars.unlinkedBodyEndColumn;
    m_constructAbility = scalars.constructAbility;
    m_startOffset = scalars.startOffset;
    m_scriptMode = scalars.scriptMode;
    m_sourceLength = scalars.sourceLength;
    m_superBinding = scalars.superBinding;
    m_parametersStartOffset = scalars.parametersStartOffset;
    m_unlinkedFunctionEnd = scalars.unlinkedFunctionEnd;
    m_needsClassFieldInitializer = scalars.needsClassFieldInitializer;
    m_parameterCount = scalars.parameterCount;
    m_privateBrandRequirement = scalars.privateBrandRequirement;
    m_constructorKind = scalars.constructorKind;
    m_sourceParseMode = scalars.sourceParseMode;
    m_implementationVisibility = static_cast<unsigned>(scalars.implementationVisibility);
    m_functionMode = scalars.functionMode;
    m_derivedContextType = scalars.derivedContextType;
    m_inlineAttribute = scalars.inlineAttribute;
    m_evalContextType = scalars.evalContextType;
    m_hasName = scalars.hasName;

    uint32_t leafExecutables = 2;
    auto checkBounds = [&](int32_t& codeBlockOffset, const CachedFunctionExecutable::CodeBlockSlot* slot) {
        if (slot && !slot->isEmpty()) {
            ptrdiff_t offset = decoder.offsetOf(slot);
            if (static_cast<size_t>(offset) < decoder.size()) {
                codeBlockOffset = offset;
                m_isCached = true;
                leafExecutables--;
                return;
            }
        }

        codeBlockOffset = 0;
    };

    if ((v.call && !v.call->isEmpty()) || (v.construct && !v.construct->isEmpty())) {
        checkBounds(m_cachedCodeBlockForCallOffset, v.call);
        checkBounds(m_cachedCodeBlockForConstructOffset, v.construct);
        if (m_isCached)
            m_decoder = &decoder;
        else
            m_decoder = nullptr;
    }

    if (leafExecutables)
        decoder.addLeafExecutable(this, decoder.offsetOf(&cachedExecutable));
}

enum CachedCodeBlockFlag : uint32_t {
    CodeBlockIsConstructorShift = 0,
    CodeBlockSuperBindingShift = 1,
    CodeBlockScriptModeShift = 2,
    CodeBlockIsArrowFunctionContextShift = 3,
    CodeBlockIsClassContextShift = 4,
    CodeBlockHasTailCallsShift = 5,
    CodeBlockHasCheckpointsShift = 6,
    CodeBlockConstructorKindShift = 7, // 2 bits
    CodeBlockDerivedContextTypeShift = 9, // 2
    CodeBlockEvalContextTypeShift = 11, // 2
    CodeBlockCodeTypeShift = 13, // 2
    CodeBlockIsBuiltinFunctionShift = 15,
    CodeBlockIsBuiltinDefaultClassConstructorShift = 16,
};

template<typename CodeBlockType>
void CachedCodeBlock<CodeBlockType>::packScalars(const UnlinkedCodeBlock& codeBlock, VarintWriter& writer)
{
    uint32_t flags = static_cast<uint32_t>(codeBlock.m_isConstructor) << CodeBlockIsConstructorShift
        | static_cast<uint32_t>(codeBlock.m_superBinding) << CodeBlockSuperBindingShift
        | static_cast<uint32_t>(codeBlock.m_scriptMode) << CodeBlockScriptModeShift
        | static_cast<uint32_t>(codeBlock.m_isArrowFunctionContext) << CodeBlockIsArrowFunctionContextShift
        | static_cast<uint32_t>(codeBlock.m_isClassContext) << CodeBlockIsClassContextShift
        | static_cast<uint32_t>(codeBlock.m_hasTailCalls) << CodeBlockHasTailCallsShift
        | static_cast<uint32_t>(codeBlock.m_hasCheckpoints) << CodeBlockHasCheckpointsShift
        | static_cast<uint32_t>(codeBlock.m_constructorKind) << CodeBlockConstructorKindShift
        | static_cast<uint32_t>(codeBlock.m_derivedContextType) << CodeBlockDerivedContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_evalContextType) << CodeBlockEvalContextTypeShift
        | static_cast<uint32_t>(codeBlock.m_codeType) << CodeBlockCodeTypeShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinFunction) << CodeBlockIsBuiltinFunctionShift
        | static_cast<uint32_t>(codeBlock.m_isBuiltinDefaultClassConstructor) << CodeBlockIsBuiltinDefaultClassConstructorShift;
    writer.u32(flags);
    writer.u8(static_cast<uint8_t>(codeBlock.m_parseMode));
    writer.u8(codeBlock.m_codeGenerationMode.toRaw());
    writer.i32(codeBlock.m_thisRegister.offset());
    writer.i32(codeBlock.m_scopeRegister.offset());
    writer.i32(codeBlock.m_numVars);
    writer.i32(codeBlock.m_numCalleeLocals);
    writer.i32(codeBlock.m_numParameters);
    writer.u32(codeBlock.m_valueProfiles.size());
    writer.u32(codeBlock.m_arrayProfiles.size());
    writer.u32(codeBlock.m_binaryArithProfiles.size());
    writer.u32(codeBlock.m_unaryArithProfiles.size());
}

template<typename CodeBlockType>
void CachedCodeBlock<CodeBlockType>::packLayout(const Layout& layout, VarintWriter& writer)
{
    writer.u8(layout.flags);
    writer.u32(layout.recordOffsetInRegion);
    if (layout.flags & LayoutHasMetadata)
        writer.u32(layout.metadataValueProfiles);
    auto array = [&](const Array& a) {
        writer.u32(a.count);
        if (a.count)
            writer.i32(a.at);
    };
    array(layout.steps);
    array(layout.instructions);
    array(layout.constants);
    array(layout.constantsSourceCodeRepresentation);
    array(layout.identifiers);
    array(layout.functionDecls);
    array(layout.functionExprs);
    if (layout.flags & LayoutHasExtras)
        writer.i32(layout.extrasAt);
}

template<typename CodeBlockType>
auto CachedCodeBlock<CodeBlockType>::readTail(const uint8_t* limit) const -> Tail
{
    Tail tail;
    VarintReader reader(tailBytes(), limit);
    Layout& layout = tail.layout;
    layout.flags = reader.u8();
    layout.recordOffsetInRegion = reader.u32();
    if (layout.flags & LayoutHasMetadata)
        layout.metadataValueProfiles = reader.u32();
    auto array = [&](Array& a) {
        a.count = reader.u32();
        if (a.count)
            a.at = reader.i32();
    };
    array(layout.steps);
    array(layout.instructions);
    array(layout.constants);
    array(layout.constantsSourceCodeRepresentation);
    array(layout.identifiers);
    array(layout.functionDecls);
    array(layout.functionExprs);
    if (layout.flags & LayoutHasExtras)
        layout.extrasAt = reader.i32();

    Scalars& s = tail.scalars;
    uint32_t flags = reader.u32();
    tail.flags = flags;
    auto bits = [&](unsigned shift, unsigned width = 1) -> unsigned { return (flags >> shift) & ((1u << width) - 1); };
    s.isConstructor = bits(CodeBlockIsConstructorShift);
    s.superBinding = bits(CodeBlockSuperBindingShift);
    s.scriptMode = bits(CodeBlockScriptModeShift);
    s.isArrowFunctionContext = bits(CodeBlockIsArrowFunctionContextShift);
    s.isClassContext = bits(CodeBlockIsClassContextShift);
    s.hasTailCalls = bits(CodeBlockHasTailCallsShift);
    s.hasCheckpoints = bits(CodeBlockHasCheckpointsShift);
    s.constructorKind = bits(CodeBlockConstructorKindShift, 2);
    s.derivedContextType = bits(CodeBlockDerivedContextTypeShift, 2);
    s.evalContextType = bits(CodeBlockEvalContextTypeShift, 2);
    s.codeType = bits(CodeBlockCodeTypeShift, 2);
    s.isBuiltinFunction = bits(CodeBlockIsBuiltinFunctionShift);
    s.isBuiltinDefaultClassConstructor = bits(CodeBlockIsBuiltinDefaultClassConstructorShift);
    s.parseMode = static_cast<SourceParseMode>(reader.u8());
    s.codeGenerationMode = OptionSet<CodeGenerationMode>::fromRaw(reader.u8());
    s.thisRegister = VirtualRegister(reader.i32());
    s.scopeRegister = VirtualRegister(reader.i32());
    s.numVars = reader.i32();
    s.numCalleeLocals = reader.i32();
    s.numParameters = reader.i32();
    s.numValueProfiles = reader.u32();
    s.numArrayProfiles = reader.u32();
    s.numBinaryArithProfiles = reader.u32();
    s.numUnaryArithProfiles = reader.u32();
    tail.end = reader.position();
    tail.intact = !reader.overran();
    return tail;
}

template<typename CodeBlockType>
bool CachedCodeBlock<CodeBlockType>::hasCoreLayout(Decoder& decoder, const Tail& tail) const
{
    // Every array and the extras aligned for their elements, which regionIsIntact has placed.
    const Layout& layout = tail.layout;
    uintptr_t begin = std::bit_cast<uintptr_t>(regionBegin(layout));
    auto isAligned = [&](const Array& array, size_t alignment) {
        return !array.count || !((begin + static_cast<intptr_t>(array.at)) % alignment);
    };
    if (!coreCodecHolds(decoder, this, sizeof(Record), alignof(Record))
        || !isAligned(layout.steps, alignof(uint32_t))
        || !isAligned(layout.constantsSourceCodeRepresentation, alignof(SourceCodeRepresentation))
        || !isAligned(layout.constants, alignof(CachedJSValue))
        || !isAligned(layout.identifiers, alignof(CachedIdentifier))
        || !isAligned(layout.functionDecls, alignof(CachedWriteBarrier<CachedFunctionExecutable>))
        || !isAligned(layout.functionExprs, alignof(CachedWriteBarrier<CachedFunctionExecutable>)))
        return false;
    if ((layout.flags & LayoutHasExtras) && ((begin + static_cast<intptr_t>(layout.extrasAt)) % alignof(CachedCodeBlockExtras)))
        return false;

    // The layout flags and packed scalars a JITCacheCore encoder writes: no checksum (section 4), and every packed field
    // within its enum's values. The code type and the constructor kind take every value their two bits hold.
    static constexpr uint32_t knownFlags = (1u << (CodeBlockIsBuiltinDefaultClassConstructorShift + 1)) - 1;
    static constexpr uint8_t knownCodeGenerationModes = static_cast<uint8_t>(CodeGenerationMode::Debugger) | static_cast<uint8_t>(CodeGenerationMode::TypeProfiler) | static_cast<uint8_t>(CodeGenerationMode::ControlFlowProfiler);
    const Scalars& scalars = tail.scalars;
    if ((layout.flags & ~(LayoutHasMetadata | LayoutHasExtras)) || (tail.flags & ~knownFlags))
        return false;
    if (scalars.derivedContextType > static_cast<unsigned>(DerivedContextType::DerivedMethodContext)
        || scalars.evalContextType > static_cast<unsigned>(EvalContextType::InstanceFieldEvalContext)
        || static_cast<unsigned>(scalars.parseMode) > static_cast<unsigned>(SourceParseMode::ClassStaticBlockMode)
        || (scalars.codeGenerationMode.toRaw() & ~knownCodeGenerationModes))
        return false;

    // The expression info's slot is filled: CachedPtr::get asserts it.
    return !!m_expressionInfo.placedTarget<CachedExpressionInfo>(decoder);
}

template<typename CodeBlockType>
auto CachedCodeBlock<CodeBlockType>::create(Encoder& encoder, const CodeBlockType& codeBlock) -> Record*
{
    ptrdiff_t regionStart = encoder.currentOffset();
    encoder.beginBlockRegion(regionStart);
    Layout layout;
    auto place = [&](Array& array, unsigned count, auto&& write) {
        array.count = count;
        if (count)
            array.at = safeCast<int32_t>(write() - regionStart);
    };

    // These three may be shared with an identical array written earlier; regionIsIntact() follows them in this order.
    {
        Encoder::ShareableArrayScope shareable(encoder);
        const UnlinkedMetadataTable& metadata = codeBlock.m_metadata.get();
        if (metadata.m_hasMetadata) {
            layout.flags |= LayoutHasMetadata;
            layout.metadataValueProfiles = metadata.m_numValueProfiles;
            auto steps = CachedMetadataSteps::compute(metadata);
            EncoderScratchCharge stepsCharge(encoder, encoderHeapBytes(steps));
            place(layout.steps, steps.size(), [&] { return encodeArrayForTail<uint32_t>(encoder, steps); });
        }
        // The stream's span, valid for an owned and for a borrowed stream: a UCB that Bun decoded from a persistent payload
        // encodes like any other, its bytes copied (SPEC-ucb.codec.md, E3).
        const JSInstructionStream& instructions = *codeBlock.m_instructions;
        place(layout.instructions, instructions.m_bytes.size(), [&] { return encodeArrayForTail<uint8_t>(encoder, instructions.m_bytes); });
        place(layout.constantsSourceCodeRepresentation, codeBlock.m_constantsSourceCodeRepresentation.size(), [&] { return encodeArrayForTail<SourceCodeRepresentation>(encoder, codeBlock.m_constantsSourceCodeRepresentation); });
    }
    place(layout.constants, codeBlock.m_constantRegisters.size(), [&] { return CachedJSValuePool::encode(encoder, codeBlock.m_constantRegisters.span()); });
    place(layout.identifiers, codeBlock.m_identifiers.size(), [&] { return encodeArrayForTail<CachedIdentifier>(encoder, codeBlock.m_identifiers); });
    // The children's slots are part of this block's bytes; the records they point at are written after the region.
    auto allocateSlots = [&](unsigned count) {
        auto result = encoder.malloc(sizeof(CachedWriteBarrier<CachedFunctionExecutable>) * count, alignof(CachedWriteBarrier<CachedFunctionExecutable>));
        static_assert(PayloadType<CachedWriteBarrier<CachedFunctionExecutable>>);
        new (result.buffer()) CachedWriteBarrier<CachedFunctionExecutable>[count];
        return result.offset();
    };
    place(layout.functionDecls, codeBlock.m_functionDecls.size(), [&] { return allocateSlots(codeBlock.m_functionDecls.size()); });
    place(layout.functionExprs, codeBlock.m_functionExprs.size(), [&] { return allocateSlots(codeBlock.m_functionExprs.size()); });
    if (CachedCodeBlockExtras::isNeeded(codeBlock)) {
        layout.flags |= LayoutHasExtras;
        auto result = encoder.malloc(sizeof(CachedCodeBlockExtras), alignof(CachedCodeBlockExtras));
        layout.extrasAt = safeCast<int32_t>(result.offset() - regionStart);
        static_assert(PayloadType<CachedCodeBlockExtras>);
        (new (result.buffer()) CachedCodeBlockExtras())->encode(encoder, codeBlock);
    }

    if (encoder.checksums())
        layout.flags |= LayoutHasChecksum;
    size_t trailerBytes = (layout.flags & LayoutHasChecksum) ? 2 * sizeof(uint32_t) : 0;
    VarintWriter writer;
    // The tail holds the record's own offset in the region as a varint, so its size is settled where it is placed.
    auto result = encoder.mallocPlaced(alignof(Record), [&](ptrdiff_t offset) {
        layout.recordOffsetInRegion = safeCast<uint32_t>(offset - regionStart);
        writer = { };
        packLayout(layout, writer);
        packScalars(codeBlock, writer);
        return sizeof(Record) + writer.size() + trailerBytes;
    });
    static_assert(PayloadType<Record>);
    Record* record = new (result.buffer()) Record();
    writer.copyTo(record->tailBytes());
    EncoderScratchCharge writerCharge(encoder, writer.heapBytes());
    ptrdiff_t trailerOffset = result.offset() + sizeof(Record) + writer.size();
    encoder.deferCold([record, &encoder, expressionInfo = codeBlock.m_expressionInfo.get()] {
        // Self-checksummed and position-independent, so an identical one written earlier is reused.
        auto bytes = CachedExpressionInfo::pack(*expressionInfo, encoder.checksums());
        EncoderScratchCharge bytesCharge(encoder, encoderHeapBytes(bytes));
        unsigned hash = StringHasher::computeHashAndMaskTop8Bits(bytes.span()) ^ static_cast<unsigned>(bytes.size());
        ptrdiff_t at;
        if (auto existing = encoder.existingIdenticalArray(bytes.span(), hash, alignof(CachedExpressionInfo)))
            at = *existing;
        else {
            auto allocation = encoder.malloc(bytes.size(), alignof(CachedExpressionInfo));
            memcpySpan(std::span { allocation.buffer(), bytes.size() }, bytes.span());
            encoder.registerArray(hash, allocation.offset(), bytes.size());
            at = allocation.offset();
        }
        record->m_expressionInfo.pointAtPayloadOffset(encoder, at);
    });
    record->encodeOwnMembers(encoder, codeBlock);

    uint32_t regionSize = safeCast<uint32_t>(encoder.currentOffset() - regionStart);
    if (trailerBytes) {
        memcpySpan(encoder.mutableBytesAt(trailerOffset, sizeof(uint32_t)), std::span { reinterpret_cast<const uint8_t*>(&regionSize), sizeof(regionSize) });
        encoder.addChecksum(regionStart, regionSize, trailerOffset + sizeof(uint32_t), encoder.takeBlockExternalArrays());
    } else
        encoder.takeBlockExternalArrays();

    auto encodeChildren = [&](const Array& slots, const auto& executables) {
        if (!slots.count)
            return;
        auto bytes = encoder.mutableBytesAt(regionStart + slots.at, sizeof(CachedWriteBarrier<CachedFunctionExecutable>) * slots.count);
        auto* slot = reinterpret_cast<CachedWriteBarrier<CachedFunctionExecutable>*>(bytes.data());
        for (unsigned i = 0; i < slots.count; ++i)
            slot[i].encode(encoder, executables[i]);
    };
    encodeChildren(layout.functionDecls, codeBlock.m_functionDecls);
    encodeChildren(layout.functionExprs, codeBlock.m_functionExprs);
    return record;
}

class CachedSourceCodeKey : public CachedObject<SourceCodeKey> {
public:
    void encode(Encoder& encoder, const SourceCodeKey& key)
    {
        m_sourceCode.encode(encoder, key.m_sourceCode);
        m_name.encode(encoder, key.m_name);
        m_flags = key.m_flags.m_flags;
        m_hash = key.hash();
        m_functionConstructorParametersEndPosition = key.m_functionConstructorParametersEndPosition;
    }

    void decode(Decoder& decoder, SourceCodeKey& key) const
    {
        m_sourceCode.decode(decoder, key.m_sourceCode);
        m_name.decode(decoder, key.m_name);
        key.m_flags.m_flags = m_flags;
        key.m_hash = m_hash;
        key.m_functionConstructorParametersEndPosition = m_functionConstructorParametersEndPosition;
    }

private:
    CachedUnlinkedSourceCode m_sourceCode;
    CachedString m_name;
    unsigned m_flags;
    unsigned m_hash;
    int m_functionConstructorParametersEndPosition;
};

class GenericCacheEntry {
public:
    bool decode(Decoder&, std::pair<SourceCodeKey, UnlinkedCodeBlock*>&) const;
    bool decode(Decoder&, SourceCodeKey&) const;
    bool isStillValid(Decoder&, const SourceCodeKey&, CachedCodeBlockTag) const;

protected:
    GenericCacheEntry(Encoder& encoder, CachedCodeBlockTag tag)
        : m_cacheVersion(computeJSCBytecodeCacheVersion())
        , m_tag(tag)
        , m_reservedCalleeLocals(CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters())
    {
        m_bootSessionUUID.encode(encoder, bootSessionUUIDString());
    }

    CachedCodeBlockTag NODELETE tag() const { return m_tag; }

    bool isUpToDate(Decoder& decoder) const
    {
        if (m_cacheVersion != computeJSCBytecodeCacheVersion())
            return false;
        // The entry, its boot session string and its source code key, up to where the code block starts.
        if (!decoder.regionChecksumMatches(this, m_headerSize, &m_headerChecksum))
            return false;
        if (m_bootSessionUUID.decode(decoder) != bootSessionUUIDString())
            return false;
        // BytecodeGenerator numbers a code block's locals after the LLInt/baseline callee-save area, so its size is baked into the bytecode.
        if (m_reservedCalleeLocals != CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters())
            return false;
        return true;
    }

    void sealHeader(Encoder& encoder)
    {
        m_headerSize = safeCast<uint32_t>(encoder.currentOffset()); // the entry is at offset 0
        encoder.addChecksum(0, m_headerSize, encoder.offsetOf(&m_headerChecksum));
    }

private:
    uint32_t m_cacheVersion;
    uint32_t m_headerSize { 0 };
    uint32_t m_headerChecksum { 0 };
    CachedString m_bootSessionUUID;
    CachedCodeBlockTag m_tag;
    uint32_t m_reservedCalleeLocals;
};

static_assert(alignof(GenericCacheEntry) <= encoderMaxAlignment);

template<typename UnlinkedCodeBlockType>
class CacheEntry : public GenericCacheEntry {
public:
    CacheEntry(Encoder& encoder)
        : GenericCacheEntry(encoder, CachedCodeBlockTypeImpl<UnlinkedCodeBlockType>::tag)
    {
    }

    void encode(Encoder& encoder, std::pair<SourceCodeKey, const UnlinkedCodeBlockType*> pair)
    {
        m_key.encode(encoder, pair.first);
        sealHeader(encoder);
        m_codeBlock.encode(encoder, pair.second);
    }

private:
    friend GenericCacheEntry;

    bool isStillValid(Decoder& decoder, const SourceCodeKey& key) const
    {
        SourceCodeKey decodedKey;
        m_key.decode(decoder, decodedKey);
        return decodedKey == key;
    }

    bool decode(Decoder& decoder, std::pair<SourceCodeKey, UnlinkedCodeBlockType*>& result) const
    {
        ASSERT(tag() == CachedCodeBlockTypeImpl<UnlinkedCodeBlockType>::tag);
        SourceCodeKey decodedKey;
        m_key.decode(decoder, decodedKey);
        result = { WTF::move(decodedKey), m_codeBlock.decode(decoder) };
        return true;
    }

    bool decode(Decoder& decoder, SourceCodeKey& key) const
    {
        m_key.decode(decoder, key);
        return true;
    }

    CachedSourceCodeKey m_key;
    CachedPtr<CachedCodeBlockType<UnlinkedCodeBlockType>> m_codeBlock;
};

static_assert(alignof(CacheEntry<UnlinkedProgramCodeBlock>) <= alignof(std::max_align_t));
static_assert(alignof(CacheEntry<UnlinkedModuleProgramCodeBlock>) <= alignof(std::max_align_t));

bool GenericCacheEntry::decode(Decoder& decoder, std::pair<SourceCodeKey, UnlinkedCodeBlock*>& result) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (m_tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->decode(decoder, reinterpret_cast<std::pair<SourceCodeKey, UnlinkedProgramCodeBlock*>&>(result));
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->decode(decoder, reinterpret_cast<std::pair<SourceCodeKey, UnlinkedModuleProgramCodeBlock*>&>(result));
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool GenericCacheEntry::decode(Decoder& decoder, SourceCodeKey& key) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (m_tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->decode(decoder, key);
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->decode(decoder, key);
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        return false;
    }

    return false;
}

bool GenericCacheEntry::isStillValid(Decoder& decoder, const SourceCodeKey& key, CachedCodeBlockTag tag) const
{
    if (!isUpToDate(decoder))
        return false;

    switch (tag) {
    case CachedCodeBlockTag::CachedProgramCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedProgramCodeBlock>*>(this)->isStillValid(decoder, key);
    case CachedCodeBlockTag::CachedModuleCodeBlockTag:
        return std::bit_cast<const CacheEntry<UnlinkedModuleProgramCodeBlock>*>(this)->isStillValid(decoder, key);
    case CachedCodeBlockTag::CachedBuiltinFunctionTag:
    case CachedCodeBlockTag::CachedEvalCodeBlockTag:
        // We do not cache eval code blocks
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

template<typename UnlinkedCodeBlockType>
void encodeCodeBlock(Encoder& encoder, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock)
{
    auto* entry = encoder.template malloc<CacheEntry<UnlinkedCodeBlockType>>(encoder);
    entry->encode(encoder, { key, uncheckedDowncast<UnlinkedCodeBlockType>(codeBlock) });
}

// A builtin function (BuiltinExecutables::createExecutable) and, lazily, its body and nested functions. The embedder
// supplies the source it was created from and a stamp identifying that source's contents; nothing is hashed at load.
class BuiltinFunctionCacheEntry : public GenericCacheEntry {
public:
    BuiltinFunctionCacheEntry(Encoder& encoder)
        : GenericCacheEntry(encoder, CachedCodeBlockTag::CachedBuiltinFunctionTag)
    {
    }

    void encode(Encoder& encoder, const UnlinkedFunctionExecutable& executable, unsigned sourceLength, unsigned embedderStamp)
    {
        m_sourceLength = sourceLength;
        m_embedderStamp = embedderStamp;
        sealHeader(encoder);
        m_executable.encode(encoder, &executable);
    }

    UnlinkedFunctionExecutable* decode(Decoder& decoder, unsigned sourceLength, unsigned embedderStamp) const
    {
        if (tag() != CachedCodeBlockTag::CachedBuiltinFunctionTag || !isUpToDate(decoder))
            return nullptr;
        if (m_sourceLength != sourceLength || m_embedderStamp != embedderStamp)
            return nullptr;
        auto* record = m_executable.getIfInPayload(decoder);
        if (!record || !record->isIntact(decoder))
            return nullptr;
        return m_executable.decode(decoder);
    }

private:
    unsigned m_sourceLength { 0 };
    unsigned m_embedderStamp { 0 };
    CachedPtr<CachedFunctionExecutable> m_executable;
};

RefPtr<CachedBytecode> encodeBuiltinFunction(VM& vm, const UnlinkedFunctionExecutable* executable, unsigned sourceLength, unsigned embedderStamp, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle, Encoder::NumberStrings::Yes, externalStrings, checksums, updatable);
    encoder.template malloc<BuiltinFunctionCacheEntry>(encoder)->encode(encoder, *executable, sourceLength, embedderStamp);
    encoder.encodeDeferred();
    return encoder.release(error);
}

UnlinkedFunctionExecutable* decodeBuiltinFunction(VM& vm, Ref<CachedBytecode> cachedBytecode, SourceProvider& provider, unsigned embedderStamp)
{
    if (cachedBytecode->span().size() < sizeof(BuiltinFunctionCacheEntry))
        return nullptr;
    unsigned sourceLength = provider.source().length();
    auto* entry = std::bit_cast<const BuiltinFunctionCacheEntry*>(cachedBytecode->span().data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &provider);
    DeferGC deferGC(vm);
    UnlinkedFunctionExecutable* executable = entry->decode(decoder.get(), sourceLength, embedderStamp);
    // JITCache: the decoded builtin is a root, whose identity is recorded before link reads it (SPEC-ucb.codec.md, E9;
    // SPEC-ucb.md section 7.2.4).
    if (executable)
        JITCache::didDecodeBuiltinExecutable(vm, *executable, provider);
    return executable;
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    const ClassInfo* classInfo = codeBlock->classInfo();

    Encoder encoder(vm, fileHandle, Encoder::NumberStrings::Yes, externalStrings, checksums, updatable);
    if (classInfo == UnlinkedProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedProgramCodeBlock>(encoder, key, codeBlock);
    else if (classInfo == UnlinkedModuleProgramCodeBlock::info())
        encodeCodeBlock<UnlinkedModuleProgramCodeBlock>(encoder, key, codeBlock);
    else
        ASSERT(classInfo == UnlinkedEvalCodeBlock::info());
    encoder.encodeDeferred();

    return encoder.release(error);
}

RefPtr<CachedBytecode> encodeCodeBlock(VM& vm, const SourceCodeKey& key, const UnlinkedCodeBlock* codeBlock, EncoderStringTable* externalStrings, BytecodeCacheChecksums checksums, BytecodeCacheUpdatable updatable)
{
    BytecodeCacheError error;
    FileSystem::FileHandle invalidFileHandle;
    return encodeCodeBlock(vm, key, codeBlock, invalidFileHandle, error, externalStrings, checksums, updatable);
}

RefPtr<CachedBytecode> encodeFunctionCodeBlock(VM& vm, const UnlinkedFunctionCodeBlock* codeBlock, BytecodeCacheError& error)
{
    FileSystem::FileHandle invalidFileHandle;
    Encoder encoder(vm, invalidFileHandle, Encoder::NumberStrings::No);
    ptrdiff_t rootOffset = encoder.offsetOf(CachedFunctionCodeBlock::create(encoder, *codeBlock));
    encoder.encodeDeferred();
    RefPtr<CachedBytecode> result = encoder.release(error);
    if (result)
        result->setRootOffset(rootOffset);
    return result;
}

std::optional<SourceCodeKey> decodeSourceCodeKey(VM& vm, Ref<CachedBytecode> cachedBytecode)
{
    const auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(cachedBytecode->span().data());
    Ref<Decoder> decoder = Decoder::create(vm, WTF::move(cachedBytecode));

    SourceCodeKey key;
    if (!cachedEntry->decode(decoder.get(), key))
        return std::nullopt;
    return key;
}
UnlinkedCodeBlock* decodeCodeBlockImpl(VM& vm, const SourceCodeKey& key, Ref<CachedBytecode> cachedBytecode)
{
    MonotonicTime before;
    size_t cachedBytecodeSize = cachedBytecode->size();
    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]]
        before = MonotonicTime::now();

    auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(cachedBytecode->span().data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &key.source().provider());
    std::pair<SourceCodeKey, UnlinkedCodeBlock*> entry;
    {
        DeferGC deferGC(vm);
        if (!cachedEntry->decode(decoder.get(), entry))
            return nullptr;
    }
    if (entry.first != key)
        return nullptr;

    if (Options::reportBytecodeCacheDecodeTimes()) [[unlikely]] {
        MonotonicTime after = MonotonicTime::now();
        dataLogLn("BytecodeCache: decoded ", key.source().provider().sourceURL(), " (", cachedBytecodeSize, " bytes) in ", (after - before).milliseconds(), " ms.");
    }

    return entry.second;
}

bool isCachedBytecodeStillValid(VM& vm, Ref<CachedBytecode> cachedBytecode, const SourceCodeKey& key, SourceCodeType type)
{
    auto span = cachedBytecode->span();
    if (span.empty())
        return false;
    auto* cachedEntry = std::bit_cast<const GenericCacheEntry*>(span.data());
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode));
    return cachedEntry->isStillValid(decoder.get(), key, tagFromSourceCodeType(type));
}


// The size of every record under every ABI we build (see PayloadType). Changing a record means changing its number here,
// and with it the serialized form.
static_assert(sizeof(GenericCacheEntry) == 24);
static_assert(sizeof(CacheEntry<UnlinkedProgramCodeBlock>) == 56);
static_assert(sizeof(CacheEntry<UnlinkedModuleProgramCodeBlock>) == 56);
static_assert(sizeof(BuiltinFunctionCacheEntry) == 36);
static_assert(sizeof(VariableLengthObjectBase) == 4);
static_assert(sizeof(CachedPtr<CachedString>) == 4);
static_assert(sizeof(CachedRefPtr<CachedUniquedStringImpl>) == 4);
static_assert(sizeof(CachedWriteBarrier<CachedFunctionExecutable>) == 4);
static_assert(sizeof(CachedVector<uint32_t>) == 8);
static_assert(sizeof(CachedArray<double>) == 4);
static_assert(sizeof(CachedOptional<CachedJSTextPosition>) == 4);
static_assert(sizeof(CachedPair<CachedRefPtr<CachedUniquedStringImpl>, CachedEntryBits<VariableEnvironmentEntry>>) == 8);
static_assert(sizeof(CachedHashSet<CachedRefPtr<CachedUniquedStringImpl>, IdentifierRepHash>) == 8);
static_assert(sizeof(CachedPrivateNameEnvironment) == 8);
static_assert(sizeof(CachedBigInt) == 12);
static_assert(sizeof(CachedBitVector) == 8);
static_assert(sizeof(CachedClassElementDefinition) == 24);
static_assert(sizeof(CachedCodeBlockExtras) == 12);
static_assert(sizeof(CachedCodeBlockRareData) == 60);
static_assert(sizeof(CachedCompactTDZEnvironment) == 12);
static_assert(sizeof(CachedCompactTDZEnvironmentMapHandle) == 4);
static_assert(sizeof(CachedEvalCodeBlock) == 40);
static_assert(sizeof(CachedExpressionInfo) == 4);
static_assert(sizeof(CachedFunctionCodeBlock) == 4);
static_assert(sizeof(CachedFunctionExecutable) == 4);
static_assert(sizeof(CachedFunctionExecutableRareData) == 4);
static_assert(sizeof(CachedHandlerInfo) == 16);
static_assert(sizeof(CachedIdentifier) == 4);
static_assert(sizeof(CachedImmutableButterfly) == 12);
static_assert(sizeof(CachedJSTextPosition) == 12);
static_assert(sizeof(CachedJSValue) == 4);
static_assert(sizeof(CachedJSValuePoolRef) == 4);
// JITCache: a declaration map also holds its hashed layout (SPEC-ucb.codec.md, E4).
static_assert(sizeof(CachedInlineMap<CachedRefPtr<CachedUniquedStringImpl, UniquedStringImpl, WTF::PackedPtrTraits<UniquedStringImpl>>, CachedEntryBits<VariableEnvironmentEntry>, VariableEnvironment::inlineMapCapacity, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits>) == 20);
static_assert(sizeof(CachedModuleCodeBlock) == 56);
static_assert(sizeof(CachedProgramCodeBlock) == 80);
static_assert(sizeof(CachedRegExp) == 16);
static_assert(sizeof(CachedScopedArgumentsTable) == 8);
static_assert(sizeof(CachedSimpleJumpTable) == 20);
static_assert(sizeof(CachedSourceCodeKey) == 28);
static_assert(sizeof(CachedSourceOrigin) == 4);
static_assert(sizeof(CachedSourceProvider) == 8);
static_assert(sizeof(CachedString) == 4);
static_assert(sizeof(CachedStringImpl) == 12);
static_assert(sizeof(CachedStringJumpTable) == 20);
static_assert(sizeof(CachedStringSourceProvider) == 36);
static_assert(sizeof(CachedSymbolTable) == 24);
static_assert(sizeof(CachedSymbolTableEntry) == 4);
static_assert(sizeof(CachedSymbolTableRareData) == 8);
static_assert(sizeof(CachedTDZEnvironmentLink) == 8);
static_assert(sizeof(CachedTemplateObjectDescriptor) == 20);
static_assert(sizeof(CachedTextPosition) == 8);
static_assert(sizeof(CachedUniquedStringImpl) == 12);
static_assert(sizeof(CachedUnlinkedSourceCode) == 12);
static_assert(sizeof(CachedVariableEnvironment) == 28);
static_assert(sizeof(CachedVariableEnvironmentRareData) == 8);
#if ENABLE(WEBASSEMBLY)
static_assert(sizeof(CachedWebAssemblySourceProvider) == 40);
#endif
void decodeFunctionCodeBlock(Decoder& decoder, int32_t cachedFunctionCodeBlockOffset, WriteBarrier<UnlinkedFunctionCodeBlock>& codeBlock, const JSCell* owner)
{
    ASSERT(decoder.vm().heap.isDeferred());
    auto* cachedCodeBlock = static_cast<const CachedWriteBarrier<CachedFunctionCodeBlock, UnlinkedFunctionCodeBlock>*>(decoder.ptrForOffsetFromBase(cachedFunctionCodeBlockOffset));
    cachedCodeBlock->decode(decoder, codeBlock, owner);
}

// JITCache: the core codec's entry points (SPEC-ucb.codec.md, section 2). A core and a descriptor start with a 16-byte
// header: a magic at offset 0, a core's kind at offset 4 (a descriptor's is 0), rootOffset at offset 8, the offset of the
// root record from the payload's start, and zero bytes elsewhere.
static constexpr uint32_t coreCodecPayloadMagic = 0x43424355;
static constexpr uint32_t coreCodecDescriptorMagic = 0x44424355;
static constexpr size_t coreCodecHeaderSize = 16;

static UnlinkedCodeBlockCoreKind coreCodecKindOf(const UnlinkedCodeBlock& codeBlock)
{
    const ClassInfo* classInfo = codeBlock.classInfo();
    if (classInfo == UnlinkedProgramCodeBlock::info())
        return UnlinkedCodeBlockCoreKind::Program;
    if (classInfo == UnlinkedModuleProgramCodeBlock::info())
        return UnlinkedCodeBlockCoreKind::Module;
    if (classInfo == UnlinkedEvalCodeBlock::info())
        return UnlinkedCodeBlockCoreKind::Eval;
    RELEASE_ASSERT(classInfo == UnlinkedFunctionCodeBlock::info());
    return UnlinkedCodeBlockCoreKind::Function;
}

// The start chain of E14: for a function core, the TDZ chain of the UFE whose slot holds the UCB; none otherwise.
static RefPtr<TDZEnvironmentLink> coreCodecStartChain(UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder)
{
    ASSERT(!holder || kind == UnlinkedCodeBlockCoreKind::Function);
    if (kind != UnlinkedCodeBlockCoreKind::Function || !holder)
        return nullptr;
    return holder->parentScopeTDZVariables();
}

static void coreCodecWriteHeader(Encoder::Allocation header, uint32_t magic, uint8_t kind, ptrdiff_t rootOffset)
{
    static_assert(std::endian::native == std::endian::little, "a core's integers are little-endian");
    ASSERT(!header.offset());
    uint32_t root = safeCast<uint32_t>(rootOffset);
    memcpy(header.buffer(), &magic, sizeof(magic));
    header.buffer()[4] = kind;
    memcpy(header.buffer() + 8, &root, sizeof(root));
}

// Section 3: the header, the root record of the UCB's class, its rootOffset, then the deferred work.
static void coreCodecEncodeCore(Encoder& encoder, const UnlinkedCodeBlock& codeBlock, UnlinkedCodeBlockCoreKind kind)
{
    // The pages are zeroed, so the header's other bytes are.
    auto header = encoder.malloc(coreCodecHeaderSize, encoderMaxAlignment);
    ptrdiff_t rootOffset = 0;
    switch (kind) {
    case UnlinkedCodeBlockCoreKind::Program:
        rootOffset = encoder.offsetOf(CachedProgramCodeBlock::create(encoder, *uncheckedDowncast<UnlinkedProgramCodeBlock>(&codeBlock)));
        break;
    case UnlinkedCodeBlockCoreKind::Module:
        rootOffset = encoder.offsetOf(CachedModuleCodeBlock::create(encoder, *uncheckedDowncast<UnlinkedModuleProgramCodeBlock>(&codeBlock)));
        break;
    case UnlinkedCodeBlockCoreKind::Eval:
        rootOffset = encoder.offsetOf(CachedEvalCodeBlock::create(encoder, *uncheckedDowncast<UnlinkedEvalCodeBlock>(&codeBlock)));
        break;
    case UnlinkedCodeBlockCoreKind::Function:
        rootOffset = encoder.offsetOf(CachedFunctionCodeBlock::create(encoder, *uncheckedDowncast<UnlinkedFunctionCodeBlock>(&codeBlock)));
        break;
    }
    coreCodecWriteHeader(header, coreCodecPayloadMagic, static_cast<uint8_t>(kind), rootOffset);
    encoder.encodeDeferred();
}

// Section 2: the UFE's record as a core writes a child, alone, with its own chain as the start chain, so that the chain is
// one start record.
static void coreCodecEncodeDescriptor(Encoder& encoder, const UnlinkedFunctionExecutable& executable)
{
    auto header = encoder.malloc(coreCodecHeaderSize, encoderMaxAlignment);
    CachedFunctionExecutable* record = encoder.mallocFor<CachedFunctionExecutable>(executable);
    record->encode(encoder, executable);
    coreCodecWriteHeader(header, coreCodecDescriptorMagic, 0, encoder.offsetOf(record));
    encoder.encodeDeferred();
}

// One JITCacheCore encode and its charges (E8). The encoder is freed before every charge it made is released; a payload
// it releases stays charged for its caller.
class CoreCodecEncode {
    WTF_MAKE_NONCOPYABLE(CoreCodecEncode);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    CoreCodecEncode(VM& vm, CoreEncodingBudget* budget, RefPtr<TDZEnvironmentLink>&& startChain)
        : m_charges { budget }
        , m_startChain(WTF::move(startChain))
        , m_encoder(vm, m_fileHandle, Encoder::NumberStrings::Yes, nullptr, BytecodeCacheChecksums::No, BytecodeCacheUpdatable::No, Encoder::Purpose::JITCacheCore, budget, m_startChain.get())
    {
    }

    // The members go in reverse order, so the encoder is freed before m_charges releases what it charged.
    ~CoreCodecEncode() { m_charges.bytes = m_encoder.chargedBytes(); }

    Encoder& encoder() { return m_encoder; }
    CoreEncodeFailure failure() const { return (m_encoder.budgetRefused() || m_payloadRefused) ? CoreEncodeFailure::BudgetRefused : CoreEncodeFailure::None; }

    // The payload, its size charged before it is allocated; null when anything was refused.
    RefPtr<CachedBytecode> release()
    {
        if (m_encoder.budgetRefused())
            return nullptr;
        if (m_charges.budget && !m_charges.budget->charge(m_encoder.releasedSize())) {
            m_payloadRefused = true;
            return nullptr;
        }
        BytecodeCacheError error;
        RefPtr<CachedBytecode> payload = m_encoder.release(error);
        RELEASE_ASSERT(payload);
        return payload;
    }

    // E13: the payload's pages in order, unless anything was refused, in which case the sink sees nothing.
    void forEachPage(const ScopedLambda<void(std::span<const uint8_t>)>& sink)
    {
        if (!m_encoder.budgetRefused())
            m_encoder.forEachPage(sink);
    }

private:
    struct Charges {
        WTF_MAKE_NONCOPYABLE(Charges);

    public:
        explicit Charges(CoreEncodingBudget* encodeBudget)
            : budget(encodeBudget)
        {
        }
        ~Charges()
        {
            if (budget && bytes)
                budget->release(bytes);
        }

        CoreEncodingBudget* const budget;
        size_t bytes { 0 };
    };

    Charges m_charges;
    const RefPtr<TDZEnvironmentLink> m_startChain;
    FileSystem::FileHandle m_fileHandle;
    Encoder m_encoder;
    bool m_payloadRefused { false };
};

RefPtr<CachedBytecode> encodeUnlinkedCodeBlockCore(VM& vm, const UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, CoreEncodingBudget* budget, CoreEncodeFailure& failure)
{
    UnlinkedCodeBlockCoreKind kind = coreCodecKindOf(codeBlock);
    CoreCodecEncode encode(vm, budget, coreCodecStartChain(kind, holder));
    coreCodecEncodeCore(encode.encoder(), codeBlock, kind);
    RefPtr<CachedBytecode> payload = encode.release();
    failure = encode.failure();
    return payload;
}

RefPtr<CachedBytecode> encodeUnlinkedFunctionExecutableDescriptor(VM& vm, const UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, CoreEncodeFailure& failure)
{
    CoreCodecEncode encode(vm, budget, executable.parentScopeTDZVariables());
    coreCodecEncodeDescriptor(encode.encoder(), executable);
    RefPtr<CachedBytecode> payload = encode.release();
    failure = encode.failure();
    return payload;
}

CoreEncodeFailure forEachUnlinkedCodeBlockCoreChunk(VM& vm, const UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, CoreEncodingBudget* budget, const ScopedLambda<void(std::span<const uint8_t>)>& sink)
{
    UnlinkedCodeBlockCoreKind kind = coreCodecKindOf(codeBlock);
    CoreCodecEncode encode(vm, budget, coreCodecStartChain(kind, holder));
    coreCodecEncodeCore(encode.encoder(), codeBlock, kind);
    encode.forEachPage(sink);
    return encode.failure();
}

CoreEncodeFailure forEachUnlinkedFunctionExecutableDescriptorChunk(VM& vm, const UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, const ScopedLambda<void(std::span<const uint8_t>)>& sink)
{
    CoreCodecEncode encode(vm, budget, executable.parentScopeTDZVariables());
    coreCodecEncodeDescriptor(encode.encoder(), executable);
    encode.forEachPage(sink);
    return encode.failure();
}

// Section 3: the root record of the requested kind, placed and aligned, decoded; null for a damaged region.
template<typename Record>
static UnlinkedCodeBlock* coreCodecDecodeRoot(Decoder& decoder, std::span<const uint8_t> payload, uint32_t rootOffset)
{
    if (rootOffset % alignof(Record) || rootOffset > payload.size() || payload.size() - rootOffset < sizeof(Record)) {
        decoder.noteFailure(CoreDecodeFailure::Malformed);
        return nullptr;
    }
    return std::bit_cast<const Record*>(payload.data() + rootOffset)->decode(decoder);
}

UnlinkedCodeBlock* decodeUnlinkedCodeBlockCore(VM& vm, Ref<CachedBytecode> cachedBytecode, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, bool validate, CoreDecodeFailure& failure)
{
    failure = CoreDecodeFailure::None;
    std::span<const uint8_t> payload = cachedBytecode->span();
    if (payload.size() < coreCodecHeaderSize || std::bit_cast<uintptr_t>(payload.data()) % encoderMaxAlignment) {
        failure = CoreDecodeFailure::Malformed;
        return nullptr;
    }
    uint32_t magic;
    uint32_t rootOffset;
    uint32_t lastWord;
    memcpy(&magic, payload.data(), sizeof(magic));
    memcpy(&rootOffset, payload.data() + 8, sizeof(rootOffset));
    memcpy(&lastWord, payload.data() + 12, sizeof(lastWord));
    if (magic != coreCodecPayloadMagic || payload[5] || payload[6] || payload[7] || lastWord) {
        failure = CoreDecodeFailure::Malformed;
        return nullptr;
    }
    if (payload[4] != static_cast<uint8_t>(kind)) {
        failure = CoreDecodeFailure::KindMismatch;
        return nullptr;
    }

    // The payload stays alive in the decoder; no UFE keeps the decoder, since a core writes no child body (E1, E7).
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), &provider, Decoder::Purpose::JITCacheCore, coreCodecStartChain(kind, holder), validate);
    UnlinkedCodeBlock* codeBlock = nullptr;
    {
        DeferGC deferGC(vm);
        switch (kind) {
        case UnlinkedCodeBlockCoreKind::Program:
            codeBlock = coreCodecDecodeRoot<CachedProgramCodeBlock>(decoder.get(), payload, rootOffset);
            break;
        case UnlinkedCodeBlockCoreKind::Module:
            codeBlock = coreCodecDecodeRoot<CachedModuleCodeBlock>(decoder.get(), payload, rootOffset);
            break;
        case UnlinkedCodeBlockCoreKind::Eval:
            codeBlock = coreCodecDecodeRoot<CachedEvalCodeBlock>(decoder.get(), payload, rootOffset);
            break;
        case UnlinkedCodeBlockCoreKind::Function:
            codeBlock = coreCodecDecodeRoot<CachedFunctionCodeBlock>(decoder.get(), payload, rootOffset);
            break;
        }
    }
    // A region CachedCodeBlock::regionIsIntact refuses decodes as null. Any failure discards the UCB, which nothing
    // references; the collector frees it.
    if (!codeBlock && decoder->failure() == CoreDecodeFailure::None)
        decoder->noteFailure(CoreDecodeFailure::Malformed);
    failure = decoder->failure();
    if (failure != CoreDecodeFailure::None)
        return nullptr;
    return codeBlock;
}

std::optional<std::array<uint8_t, 32>> tdzChainDigest(const TDZEnvironmentLink* chain, CoreEncodingBudget* budget, unsigned& environmentsDigested)
{
    return CachedTDZEnvironmentLink::chainDigest(chain, budget, environmentsDigested);
}

#if ENABLE(JITCACHE_TWINS)

// JITCache: the cases of U7 that alter a core's records (SPEC-ucb.codec.md, section 7). Each encodes a core, changes one
// record of a copy through the record's type, and decodes the copy, which must fail as the rule names and not crash.
struct CoreCodecSelfTestAccess {
    // A program whose core holds each record a case alters: more than nine global vars, so the var declarations take
    // hashed storage, and an unreferenced one, whose name only that map holds; two lexical declarations in inline storage;
    // a block whose captured binding makes a SymbolTable constant; immutable butterflies of strings, of int32s and of
    // doubles; a butterfly of the NaN the parser folds from 1e400 * 0 and of its negation, whose Double elements keep the
    // CPU's bits, so that the unaltered core shows that the validating decode accepts the NaNs of both signs generation
    // writes; a BigInt, a RegExp, a double, long string constants and a template object descriptor; and two arrow
    // functions, the one that captures the binding and a template literal's tag.
    static constexpr ASCIILiteral programText =
        "var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11;\n"
        "var unreferencedVariable;\n"
        "let lexicalFirst = \"first lexical\", lexicalSecond = \"second lexical\";\n"
        "{\n"
        "    let capturedBinding = \"captured binding\";\n"
        "    globalThis.capturedReader = () => capturedBinding;\n"
        "}\n"
        "var words = [\"alphabet\", \"betamax\", \"gammaray\"];\n"
        "var numbers = [1, 2, 3];\n"
        "var fractions = [1.5, 2.5];\n"
        "var foldedNaNs = [1e400 * 0, -(1e400 * 0)];\n"
        "var big = 123456789012345678901234567890n;\n"
        "var pattern = /ab+c/g;\n"
        "var text = \"a long string constant\";\n"
        "var tagged = ((strings) => strings)`tagged ${v1} literal`;\n"
        "if (text === \"another long string\")\n"
        "    v0 = 0.25;\n"_s;

    // A method created while its class's binding is under TDZ, so that its UFE holds a TDZ chain, and an arrow created
    // while the method's own bindings are under TDZ, whose chain is a link naming them over the method's chain: the
    // method's core holds that link, its environment and a start record (E14).
    static constexpr ASCIILiteral functionText =
        "class CoreCodecFixture {\n"
        "    method()\n"
        "    {\n"
        "        const reader = () => CoreCodecFixture === pending;\n"
        "        let pending = reader;\n"
        "        return reader;\n"
        "    }\n"
        "}\n"_s;

    template<typename T, typename Source>
    static T* target(const VariableLengthObject<Source>& slot)
    {
        if (slot.isEmpty())
            return nullptr;
        return const_cast<T*>(slot.template buffer<T>());
    }

    template<typename T, size_t inlineCapacity, typename OverflowHandler, typename Malloc>
    static std::span<T> elements(const CachedVector<T, inlineCapacity, OverflowHandler, Malloc>& vector)
    {
        if (!vector.m_size)
            return { };
        return { target<T>(vector), vector.m_size };
    }

    template<typename T, typename Source>
    static CachedUniquedStringImpl* stringRecord(const CachedPtr<T, Source>& slot)
    {
        if (slot.isEmpty() || slot.hasInlineString() || slot.hasExternalString())
            return nullptr;
        return target<CachedUniquedStringImpl>(slot);
    }

    static bool hasCharacters(const CachedUniquedStringImpl& record, ASCIILiteral characters)
    {
        return record.m_is8Bit && !record.m_isSymbol && equalSpans(record.span8(), characters.span8());
    }

    template<typename Pairs>
    static CachedUniquedStringImpl* keyRecord(Pairs pairs, ASCIILiteral name)
    {
        for (auto& pair : pairs) {
            auto* record = stringRecord(pair.m_first.ptr());
            if (record && hasCharacters(*record, name))
                return record;
        }
        return nullptr;
    }

    template<typename Record>
    static Record& root(std::span<uint8_t> payload)
    {
        uint32_t rootOffset;
        memcpy(&rootOffset, payload.data() + 8, sizeof(rootOffset));
        return *std::bit_cast<Record*>(payload.data() + rootOffset);
    }

    // One of the arrays a code block's layout locates.
    template<typename Element, typename Record, typename ArrayOf>
    static std::span<Element> array(Record& record, const ArrayOf& arrayOf)
    {
        auto tail = record.readTail();
        const auto& located = arrayOf(tail.layout);
        if (!located.count)
            return { };
        return { const_cast<Element*>(record.template at<Element>(tail.layout, located)), located.count };
    }

    template<typename Record>
    static std::span<CachedIdentifier> identifiers(Record& record)
    {
        return array<CachedIdentifier>(record, [](const auto& layout) -> const auto& { return layout.identifiers; });
    }

    template<typename Record>
    static std::span<uint8_t> instructions(Record& record)
    {
        return array<uint8_t>(record, [](const auto& layout) -> const auto& { return layout.instructions; });
    }

    // The first identifier whose string is a record rather than an inline string.
    template<typename Record>
    static CachedUniquedStringImpl* identifierRecord(Record& record, ASCIILiteral name = { })
    {
        for (auto& identifier : identifiers(record)) {
            auto* string = stringRecord(identifier.m_string.m_impl.ptr());
            if (string && (name.isNull() || hasCharacters(*string, name)))
                return string;
        }
        return nullptr;
    }

    // The record of the first constant of `kind` that `accept` takes; a string constant held in its slot has none.
    template<typename T, typename Record, typename Accept>
    static T* constantRecord(Record& record, CachedJSValue::Kind kind, const Accept& accept)
    {
        auto tail = record.readTail();
        unsigned count = tail.layout.constants.count;
        if (!count)
            return nullptr;
        auto* kinds = const_cast<uint8_t*>(record.template at<uint8_t>(tail.layout, tail.layout.constants));
        auto* slots = const_cast<CachedJSValue*>(CachedJSValuePool::slots(kinds, count));
        for (unsigned i = 0; i < count; ++i) {
            if (kinds[i] != static_cast<uint8_t>(kind))
                continue;
            if (kind == CachedJSValue::Kind::String && (slots[i].hasInlineString() || slots[i].hasExternalString()))
                continue;
            if (T* found = target<T>(slots[i]); found && accept(*found))
                return found;
        }
        return nullptr;
    }

    template<typename T, typename Record>
    static T* constantRecord(Record& record, CachedJSValue::Kind kind)
    {
        return constantRecord<T>(record, kind, [](const T&) {
            return true;
        });
    }

    template<typename Record>
    static std::span<uint8_t> constantKinds(Record& record)
    {
        return array<uint8_t>(record, [](const auto& layout) -> const auto& { return layout.constants; });
    }

    // The element kinds of the first immutable butterfly constant whose indexing type is `indexingType`, which holds JSValues.
    template<typename Record>
    static std::span<uint8_t> butterflyElementKinds(Record& record, IndexingType indexingType)
    {
        ASSERT(!hasDouble(indexingType));
        auto* butterfly = constantRecord<CachedImmutableButterfly>(record, CachedJSValue::Kind::ImmutableButterfly, [&](const CachedImmutableButterfly& candidate) {
            return candidate.m_indexingType == indexingType;
        });
        if (!butterfly || !butterfly->m_length)
            return { };
        return { target<uint8_t>(butterfly->m_cachedValues), butterfly->m_length };
    }

    // The elements of the first immutable butterfly constant that holds raw doubles.
    template<typename Record>
    static std::span<double> butterflyDoubles(Record& record)
    {
        auto* butterfly = constantRecord<CachedImmutableButterfly>(record, CachedJSValue::Kind::ImmutableButterfly, [](const CachedImmutableButterfly& candidate) {
            return candidate.m_indexingType == CopyOnWriteArrayWithDouble;
        });
        double* elements = butterfly && butterfly->m_length ? target<double>(butterfly->m_cachedDoubles) : nullptr;
        if (!elements)
            return { };
        return { elements, butterfly->m_length };
    }

    // The bits of the first Double element in the pool of an immutable butterfly constant that holds JSValues.
    template<typename Record>
    static EncodedJSValue* butterflyDoubleElement(Record& record)
    {
        EncodedJSValue* element = nullptr;
        constantRecord<CachedImmutableButterfly>(record, CachedJSValue::Kind::ImmutableButterfly, [&](const CachedImmutableButterfly& butterfly) {
            if (hasDouble(butterfly.m_indexingType) || !butterfly.m_length)
                return false;
            auto* kinds = target<uint8_t>(butterfly.m_cachedValues);
            auto* slots = CachedJSValuePool::slots(kinds, butterfly.m_length);
            for (unsigned i = 0; i < butterfly.m_length; ++i) {
                if (kinds[i] == static_cast<uint8_t>(CachedJSValue::Kind::Double)) {
                    element = target<EncodedJSValue>(slots[i]);
                    return !!element;
                }
            }
            return false;
        });
        return element;
    }

    template<typename Record>
    static std::span<CachedWriteBarrier<CachedFunctionExecutable>> functionExpressions(Record& record)
    {
        return array<CachedWriteBarrier<CachedFunctionExecutable>>(record, [](const auto& layout) -> const auto& { return layout.functionExprs; });
    }

    // Writes into `slot` the offset of `record`, as the encoder points a slot at a record.
    template<typename Source>
    static void pointAt(VariableLengthObject<Source>& slot, const void* record)
    {
        slot.setRawSlot(static_cast<uint32_t>(std::bit_cast<uintptr_t>(record) - std::bit_cast<uintptr_t>(&slot)));
    }

    // The executable record of a function core's first function expression, and the records on its TDZ chain.
    static CachedFunctionExecutable* firstFunctionExpression(std::span<uint8_t> payload)
    {
        auto slots = functionExpressions(root<CachedFunctionCodeBlock>(payload));
        if (slots.empty())
            return nullptr;
        return target<CachedFunctionExecutable>(slots[0].ptr());
    }

    static CachedTDZEnvironmentLink* firstChainRecord(std::span<uint8_t> payload)
    {
        auto* executable = firstFunctionExpression(payload);
        if (!executable)
            return nullptr;
        auto view = executable->view();
        if (!view.tdz)
            return nullptr;
        return target<CachedTDZEnvironmentLink>(view.tdz->ptr());
    }

    // The first link of that chain when it is a link the core's own generation created, with an environment.
    static CachedTDZEnvironmentLink* ownLink(std::span<uint8_t> payload)
    {
        auto* link = firstChainRecord(payload);
        if (!link || link->m_handle.isEmpty())
            return nullptr;
        return link;
    }

    static CachedCompactTDZEnvironment* ownEnvironment(std::span<uint8_t> payload)
    {
        auto* link = ownLink(payload);
        return link ? target<CachedCompactTDZEnvironment>(link->m_handle.m_environment) : nullptr;
    }

    // The first start record on that chain.
    static CachedTDZEnvironmentLink* startRecord(std::span<uint8_t> payload)
    {
        // A malformed chain cannot reach this point: the fixture's core is the encoder's own.
        for (auto* link = firstChainRecord(payload); link; link = target<CachedTDZEnvironmentLink>(link->m_parent.ptr())) {
            if (link->m_handle.isEmpty())
                return link;
        }
        return nullptr;
    }

    static ASCIILiteral failureName(CoreDecodeFailure failure)
    {
        switch (failure) {
        case CoreDecodeFailure::None:
            return "None"_s;
        case CoreDecodeFailure::Malformed:
            return "Malformed"_s;
        case CoreDecodeFailure::KindMismatch:
            return "KindMismatch"_s;
        case CoreDecodeFailure::UnresolvedSymbol:
            return "UnresolvedSymbol"_s;
        case CoreDecodeFailure::InconsistentMapLayout:
            return "InconsistentMapLayout"_s;
        }
        return "an unknown failure"_s;
    }

    // Decodes a copy of `core` that `alter` changed, which must fail with `expected`.
    template<typename Alter>
    static bool expect(VM& vm, String& failure, ASCIILiteral name, const CachedBytecode& core, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, bool validate, CoreDecodeFailure expected, const Alter& alter)
    {
        auto copy = MallocSpan<uint8_t, VMMalloc>::malloc(core.size());
        memcpySpan(copy.mutableSpan(), core.span());
        if (!alter(copy.mutableSpan())) {
            failure = makeString("U7: the codec fixture has no record for the case "_s, name);
            return false;
        }
        CoreDecodeFailure result = CoreDecodeFailure::None;
        UnlinkedCodeBlock* decoded = decodeUnlinkedCodeBlockCore(vm, CachedBytecode::create(WTF::move(copy), { }), provider, kind, holder, validate, result);
        if (result == expected && (expected == CoreDecodeFailure::None) == !!decoded)
            return true;
        failure = makeString("U7: "_s, name, validate ? " with validate"_s : " without validate"_s, " decoded to "_s, failureName(result), " instead of "_s, failureName(expected));
        return false;
    }

    template<typename Alter>
    static bool expectBothModes(VM& vm, String& failure, ASCIILiteral name, const CachedBytecode& core, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, CoreDecodeFailure expected, const Alter& alter)
    {
        return expect(vm, failure, name, core, provider, kind, holder, false, expected, alter)
            && expect(vm, failure, name, core, provider, kind, holder, true, expected, alter);
    }

    static bool runProgramCases(VM& vm, String& failure, const CachedBytecode& core, SourceProvider& provider)
    {
        using Program = CachedProgramCodeBlock;
        constexpr auto kind = UnlinkedCodeBlockCoreKind::Program;
        auto unaltered = [](std::span<uint8_t>) {
            return true;
        };
        auto malformed = [&](ASCIILiteral name, const auto& alter) {
            return expect(vm, failure, name, core, provider, kind, nullptr, true, CoreDecodeFailure::Malformed, alter);
        };
        auto hashedMap = [](Program& record) -> auto& { return record.m_varDeclarations.m_map; };
        auto inlineMap = [](Program& record) -> auto& { return record.m_lexicalDeclarations.m_map; };
        auto truncate = [](CachedUniquedStringImpl* record) {
            if (!record)
                return false;
            record->m_length = (1u << 27) - 1;
            return true;
        };

        // The fixture decodes, in both modes.
        if (!expectBothModes(vm, failure, "the unaltered program core"_s, core, provider, kind, nullptr, CoreDecodeFailure::None, unaltered))
            return false;

        // E4.
        if (!expectBothModes(vm, failure, "a hashed declaration map with a deleted bucket"_s, core, provider, kind, nullptr, CoreDecodeFailure::InconsistentMapLayout, [&](std::span<uint8_t> payload) {
            auto& map = hashedMap(root<Program>(payload));
            auto slots = elements(map.m_slots);
            if (!map.m_hashedCapacity || slots.empty())
                return false;
            slots[0] |= 1;
            return true;
        }))
            return false;
        if (!expectBothModes(vm, failure, "a hashed declaration map whose capacity exceeds minLoadInverse times its entries"_s, core, provider, kind, nullptr, CoreDecodeFailure::InconsistentMapLayout, [&](std::span<uint8_t> payload) {
            auto& map = hashedMap(root<Program>(payload));
            if (!map.m_hashedCapacity)
                return false;
            map.m_hashedCapacity = 1u << 30;
            return true;
        }))
            return false;

        // E5: a private name this VM lacks, without and with validation.
        if (!expectBothModes(vm, failure, "a payload naming an absent private name"_s, core, provider, kind, nullptr, CoreDecodeFailure::UnresolvedSymbol, [&](std::span<uint8_t> payload) {
            auto* record = identifierRecord(root<Program>(payload), "capturedReader"_s);
            if (!record)
                return false;
            record->m_isSymbol = 1;
            record->m_isPrivate = 1;
            record->m_isRegistered = 0;
            record->m_isWellKnownSymbol = 0;
            record->m_ordinal = CachedUniquedStringImpl::noOrdinal;
            return true;
        }))
            return false;

        // E15: placement.
        if (!malformed("a nested offset past the payload"_s, [&](std::span<uint8_t> payload) {
            for (auto& identifier : identifiers(root<Program>(payload))) {
                auto& slot = identifier.m_string.m_impl.ptr();
                if (!stringRecord(slot))
                    continue;
                uintptr_t past = std::bit_cast<uintptr_t>(payload.data() + payload.size()) + 64;
                slot.setRawSlot(static_cast<uint32_t>((past - std::bit_cast<uintptr_t>(&slot)) & ~static_cast<uintptr_t>(3)));
                return true;
            }
            return false;
        }))
            return false;
        if (!malformed("a vector count past the payload"_s, [&](std::span<uint8_t> payload) {
            auto& entries = hashedMap(root<Program>(payload)).m_entries;
            if (!entries.m_size)
                return false;
            entries.m_size = 1u << 28;
            return true;
        }))
            return false;
        if (!malformed("a truncated string"_s, [&](std::span<uint8_t> payload) {
            return truncate(identifierRecord(root<Program>(payload)));
        }))
            return false;
        if (!malformed("a truncated string as the key of a hashed declaration map"_s, [&](std::span<uint8_t> payload) {
            auto& map = hashedMap(root<Program>(payload));
            return map.m_hashedCapacity && truncate(keyRecord(elements(map.m_entries), "unreferencedVariable"_s));
        }))
            return false;
        if (!malformed("a truncated string as the key of an inline declaration map"_s, [&](std::span<uint8_t> payload) {
            auto& map = inlineMap(root<Program>(payload));
            return !map.m_hashedCapacity && truncate(keyRecord(elements(map.m_entries), "lexicalFirst"_s));
        }))
            return false;
        if (!malformed("a truncated string as the key of a symbol table"_s, [&](std::span<uint8_t> payload) {
            auto* table = constantRecord<CachedSymbolTable>(root<Program>(payload), CachedJSValue::Kind::SymbolTable);
            return table && truncate(keyRecord(elements(table->m_map.m_entries), "capturedBinding"_s));
        }))
            return false;
        // The record cache: an executable record, which the encoder writes once per child slot, named by two slots.
        if (!malformed("two function slots naming one executable record"_s, [&](std::span<uint8_t> payload) {
            auto slots = functionExpressions(root<Program>(payload));
            if (slots.size() < 2)
                return false;
            auto* first = target<CachedFunctionExecutable>(slots[0].ptr());
            if (!first)
                return false;
            pointAt(const_cast<std::remove_cvref_t<decltype(slots[1].ptr())>&>(slots[1].ptr()), first);
            return true;
        }))
            return false;

        // E15: strings.
        if (!malformed("a wrong stored hash"_s, [&](std::span<uint8_t> payload) {
            auto* record = identifierRecord(root<Program>(payload));
            if (!record)
                return false;
            record->m_hash ^= 1;
            return true;
        }))
            return false;
        if (!malformed("a known ordinal with other characters"_s, [&](std::span<uint8_t> payload) {
            CachedUniquedStringImpl* first = nullptr;
            for (auto& identifier : identifiers(root<Program>(payload))) {
                auto* record = stringRecord(identifier.m_string.m_impl.ptr());
                if (!record || record->m_ordinal == CachedUniquedStringImpl::noOrdinal)
                    continue;
                if (!first) {
                    first = record;
                    continue;
                }
                // Records are written once per content, so two of them hold other characters.
                record->m_ordinal = first->m_ordinal;
                return true;
            }
            return false;
        }))
            return false;
        if (!malformed("a string record without its ordinal"_s, [&](std::span<uint8_t> payload) {
            auto* record = identifierRecord(root<Program>(payload));
            if (!record)
                return false;
            record->m_ordinal = CachedUniquedStringImpl::noOrdinal;
            return true;
        }))
            return false;
        if (!malformed("a symbol record with an ordinal"_s, [&](std::span<uint8_t> payload) {
            auto* record = identifierRecord(root<Program>(payload));
            if (!record || record->m_ordinal == CachedUniquedStringImpl::noOrdinal)
                return false;
            // A registered private symbol, well formed but for the ordinal, which decode() would read before the symbol bit.
            record->m_isSymbol = 1;
            record->m_isRegistered = 1;
            record->m_isPrivate = 1;
            record->m_isWellKnownSymbol = 0;
            return true;
        }))
            return false;

        // E15: tags.
        if (!malformed("an unknown constant kind"_s, [&](std::span<uint8_t> payload) {
            auto kinds = constantKinds(root<Program>(payload));
            if (kinds.empty())
                return false;
            kinds[0] = 0xff;
            return true;
        }))
            return false;
        if (!malformed("an external-string slot"_s, [&](std::span<uint8_t> payload) {
            for (auto& identifier : identifiers(root<Program>(payload))) {
                auto& slot = identifier.m_string.m_impl.ptr();
                if (!stringRecord(slot))
                    continue;
                slot.setRawSlot(VariableLengthObjectBase::externalStringTag);
                return true;
            }
            return false;
        }))
            return false;
        if (!malformed("a symbol table entry without its slim flag"_s, [&](std::span<uint8_t> payload) {
            auto* table = constantRecord<CachedSymbolTable>(root<Program>(payload), CachedJSValue::Kind::SymbolTable);
            if (!table)
                return false;
            auto entries = elements(table->m_map.m_entries);
            if (entries.empty())
                return false;
            entries[0].m_second.m_bits &= ~CachedSymbolTableEntry::slimFlag();
            return true;
        }))
            return false;
        if (!malformed("a butterfly whose indexing type is not copy-on-write"_s, [&](std::span<uint8_t> payload) {
            auto* butterfly = constantRecord<CachedImmutableButterfly>(root<Program>(payload), CachedJSValue::Kind::ImmutableButterfly);
            if (!butterfly)
                return false;
            butterfly->m_indexingType = ArrayWithContiguous;
            return true;
        }))
            return false;
        if (!malformed("a string constant whose record is a symbol"_s, [&](std::span<uint8_t> payload) {
            auto* record = constantRecord<CachedUniquedStringImpl>(root<Program>(payload), CachedJSValue::Kind::String, [](const CachedUniquedStringImpl& string) {
                return hasCharacters(string, "a long string constant"_s);
            });
            if (!record)
                return false;
            // A private name this VM lacks, which would decode as UnresolvedSymbol past the check.
            record->m_isSymbol = 1;
            record->m_isPrivate = 1;
            record->m_ordinal = CachedUniquedStringImpl::noOrdinal;
            return true;
        }))
            return false;
        if (!malformed("a RegExp record with m_parsed set"_s, [&](std::span<uint8_t> payload) {
            auto* regExp = constantRecord<CachedRegExp>(root<Program>(payload), CachedJSValue::Kind::RegExp);
            if (!regExp)
                return false;
            regExp->m_parsed = 1;
            return true;
        }))
            return false;
        if (!malformed("a UFE header with HasCallSlot"_s, [&](std::span<uint8_t> payload) {
            auto slots = functionExpressions(root<Program>(payload));
            if (slots.empty())
                return false;
            auto* executable = target<CachedFunctionExecutable>(slots[0].ptr());
            if (!executable)
                return false;
            executable->m_header |= CachedFunctionExecutable::HasCallSlot;
            return true;
        }))
            return false;

        // E15: native assertions.
        if (!malformed("metadata steps out of order"_s, [&](std::span<uint8_t> payload) {
            auto steps = array<uint32_t>(root<Program>(payload), [](const auto& layout) -> const auto& { return layout.steps; });
            if (steps.size() < 2)
                return false;
            std::swap(steps[0], steps[1]);
            return true;
        }))
            return false;
        if (!malformed("a JSBigInt past maxLength"_s, [&](std::span<uint8_t> payload) {
            auto* bigInt = constantRecord<CachedBigInt>(root<Program>(payload), CachedJSValue::Kind::BigInt);
            if (!bigInt)
                return false;
            bigInt->m_length = CachedBigInt::maximumLength() + 1;
            return true;
        }))
            return false;

        // E15: records.
        if (!malformed("a butterfly element that is a butterfly"_s, [&](std::span<uint8_t> payload) {
            auto kinds = butterflyElementKinds(root<Program>(payload), CopyOnWriteArrayWithContiguous);
            if (kinds.empty())
                return false;
            kinds[0] = static_cast<uint8_t>(CachedJSValue::Kind::ImmutableButterfly);
            return true;
        }))
            return false;
        if (!malformed("an Undefined element in a butterfly's pool"_s, [&](std::span<uint8_t> payload) {
            auto kinds = butterflyElementKinds(root<Program>(payload), CopyOnWriteArrayWithContiguous);
            if (kinds.empty())
                return false;
            kinds[0] = static_cast<uint8_t>(CachedJSValue::Kind::Undefined);
            return true;
        }))
            return false;
        if (!malformed("a Double element in a CopyOnWriteArrayWithInt32 butterfly"_s, [&](std::span<uint8_t> payload) {
            auto kinds = butterflyElementKinds(root<Program>(payload), CopyOnWriteArrayWithInt32);
            if (kinds.empty())
                return false;
            kinds[0] = static_cast<uint8_t>(CachedJSValue::Kind::Double);
            return true;
        }))
            return false;
        // The pure NaN, which boxes as a double whose negation is pure too, so that only the rule for double storage, which
        // reads any NaN as a hole, rejects it.
        if (!malformed("a NaN element in a CopyOnWriteArrayWithDouble butterfly"_s, [&](std::span<uint8_t> payload) {
            auto elements = butterflyDoubles(root<Program>(payload));
            if (elements.empty())
                return false;
            elements[0] = PNaN;
            return true;
        }))
            return false;
        if (!malformed("a double constant whose bits name a cell"_s, [&](std::span<uint8_t> payload) {
            auto* bits = constantRecord<EncodedJSValue>(root<Program>(payload), CachedJSValue::Kind::Double);
            if (!bits)
                return false;
            *bits = static_cast<EncodedJSValue>(0x10000);
            return true;
        }))
            return false;
        // A NaN that boxes as a double but whose negation, 0xffff000000000000, is impure: the baseline's negate fast path,
        // which flips bit 63 of the boxed value, would turn it into bits that name a cell.
        auto holdDoubleWithImpureNegation = [](EncodedJSValue* bits) {
            if (!bits)
                return false;
            JSValue value(JSValue::EncodeAsDouble, std::bit_cast<double>(static_cast<uint64_t>(0x7fff000000000000ULL)));
            *bits = JSValue::encode(value);
            return value.isDouble() && isImpureNaN(-value.asDouble());
        };
        if (!malformed("a Double constant register whose negation is an impure NaN"_s, [&](std::span<uint8_t> payload) {
            return holdDoubleWithImpureNegation(constantRecord<EncodedJSValue>(root<Program>(payload), CachedJSValue::Kind::Double));
        }))
            return false;
        if (!malformed("a Double butterfly element whose negation is an impure NaN"_s, [&](std::span<uint8_t> payload) {
            return holdDoubleWithImpureNegation(butterflyDoubleElement(root<Program>(payload)));
        }))
            return false;
        if (!malformed("a template object descriptor with a null raw string"_s, [&](std::span<uint8_t> payload) {
            auto* descriptor = constantRecord<CachedTemplateObjectDescriptor>(root<Program>(payload), CachedJSValue::Kind::TemplateObjectDescriptor);
            if (!descriptor)
                return false;
            auto rawStrings = elements(descriptor->m_rawStrings);
            if (rawStrings.empty())
                return false;
            rawStrings[0].m_impl.ptr().setRawSlot(VariableLengthObjectBase::emptySentinel);
            return true;
        }))
            return false;
        if (!malformed("a template object descriptor with fewer cooked strings than raw ones"_s, [&](std::span<uint8_t> payload) {
            auto* descriptor = constantRecord<CachedTemplateObjectDescriptor>(root<Program>(payload), CachedJSValue::Kind::TemplateObjectDescriptor);
            if (!descriptor || !descriptor->m_cookedStrings.m_size)
                return false;
            --descriptor->m_cookedStrings.m_size;
            return true;
        }))
            return false;
        if (!malformed("expression info with a flag"_s, [&](std::span<uint8_t> payload) {
            auto* info = target<uint8_t>(root<Program>(payload).m_expressionInfo);
            if (!info)
                return false;
            // The flags byte follows the 32-bit count of encoded entries.
            info[sizeof(uint32_t)] |= CachedExpressionInfo::HasChecksum;
            return true;
        }))
            return false;

        // E15: instructions.
        if (!malformed("an unknown opcode"_s, [&](std::span<uint8_t> payload) {
            auto bytes = instructions(root<Program>(payload));
            if (bytes.empty())
                return false;
            bytes[0] = static_cast<uint8_t>(NUMBER_OF_BYTECODE_IDS);
            return true;
        }))
            return false;
        if (!malformed("a prefix after a prefix"_s, [&](std::span<uint8_t> payload) {
            auto bytes = instructions(root<Program>(payload));
            if (bytes.size() < 2)
                return false;
            bytes[0] = static_cast<uint8_t>(op_wide16);
            bytes[1] = static_cast<uint8_t>(op_wide16);
            return true;
        }))
            return false;
        return malformed("an instruction that runs past the stream"_s, [&](std::span<uint8_t> payload) {
            auto bytes = instructions(root<Program>(payload));
            if (bytes.empty() || !coreCodecIsWholeInstructionStream(bytes))
                return false;
            // The longest opcode, written over the opcode of the stream's last instruction.
            unsigned longest = 0;
            for (unsigned opcode = 0; opcode < NUMBER_OF_BYTECODE_IDS; ++opcode) {
                if (opcode != op_wide16 && opcode != op_wide32 && opcodeLengths[opcode] > opcodeLengths[longest])
                    longest = opcode;
            }
            size_t last = 0;
            for (size_t offset = 0; offset < bytes.size(); offset += std::bit_cast<const JSInstruction*>(bytes.data() + offset)->size())
                last = offset;
            size_t lastSize = bytes.size() - last;
            bool isPrefixed = bytes[last] == op_wide16 || bytes[last] == op_wide32;
            size_t opcodeAt = last + (isPrefixed ? 1 : 0);
            bytes[opcodeAt] = static_cast<uint8_t>(longest);
            return std::bit_cast<const JSInstruction*>(bytes.data() + last)->size() > lastSize;
        });
    }

    static bool runFunctionCases(VM& vm, String& failure, const CachedBytecode& core, SourceProvider& provider, const UnlinkedFunctionExecutable& holder)
    {
        constexpr auto kind = UnlinkedCodeBlockCoreKind::Function;
        auto unaltered = [](std::span<uint8_t>) {
            return true;
        };
        auto malformed = [&](ASCIILiteral name, const auto& alter) {
            return expect(vm, failure, name, core, provider, kind, &holder, true, CoreDecodeFailure::Malformed, alter);
        };

        if (!expectBothModes(vm, failure, "the unaltered function core"_s, core, provider, kind, &holder, CoreDecodeFailure::None, unaltered))
            return false;

        // E14.
        if (!expectBothModes(vm, failure, "a start record decoded without a start chain"_s, core, provider, kind, nullptr, CoreDecodeFailure::Malformed, [&](std::span<uint8_t> payload) {
            return !!startRecord(payload);
        }))
            return false;
        if (!expectBothModes(vm, failure, "a start record with an empty handle and a nonempty parent"_s, core, provider, kind, &holder, CoreDecodeFailure::Malformed, [&](std::span<uint8_t> payload) {
            auto* link = startRecord(payload);
            if (!link)
                return false;
            // The parent names the record itself, which a decode that followed it would loop on.
            pointAt(link->m_parent.ptr(), link);
            return true;
        }))
            return false;

        // E15: records. A decode without validate would recurse along this loop without end.
        if (!malformed("a TDZ link whose parent is itself"_s, [&](std::span<uint8_t> payload) {
            auto* link = ownLink(payload);
            if (!link)
                return false;
            pointAt(link->m_parent.ptr(), link);
            return true;
        }))
            return false;
        if (!malformed("a TDZ environment whose stored hash is not its names' hash"_s, [&](std::span<uint8_t> payload) {
            auto* environment = ownEnvironment(payload);
            if (!environment)
                return false;
            environment->m_hash ^= 1;
            return true;
        }))
            return false;
        if (!malformed("a TDZ environment with a null name"_s, [&](std::span<uint8_t> payload) {
            auto* environment = ownEnvironment(payload);
            if (!environment)
                return false;
            auto names = elements(environment->m_variables);
            if (names.empty())
                return false;
            names[0].ptr().setRawSlot(VariableLengthObjectBase::emptySentinel);
            return true;
        }))
            return false;

        // E15: placement. The function's name decodes before its chain, as a string; the record cache refuses it as an
        // environment, which a decoder caching objects by offset alone would take for one and assert on.
        return malformed("a TDZ environment slot naming a string record"_s, [&](std::span<uint8_t> payload) {
            auto* link = ownLink(payload);
            auto* executable = firstFunctionExpression(payload);
            if (!link || !executable)
                return false;
            auto view = executable->view();
            CachedUniquedStringImpl* name = view.name ? stringRecord(view.name->m_string.m_impl.ptr()) : nullptr;
            if (!name)
                return false;
            pointAt(link->m_handle.m_environment, name);
            return true;
        });
    }

    static bool run(VM& vm, String& failure)
    {
        JSLockHolder locker(vm);

        SourceCode programSource = makeSource(String { programText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        ParserError error;
        Strong<UnlinkedProgramCodeBlock> program { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, programSource, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
        if (!program.get() || error.isValid()) {
            failure = "U7: the codec fixture program failed to generate"_s;
            return false;
        }
        CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> programCore = encodeUnlinkedCodeBlockCore(vm, *program.get(), nullptr, nullptr, encodeFailure);
        if (!programCore) {
            failure = "U7: the codec fixture program's core failed to encode"_s;
            return false;
        }
        if (!runProgramCases(vm, failure, *programCore, *programSource.provider()))
            return false;

        SourceCode functionSource = makeSource(String { functionText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<UnlinkedProgramCodeBlock> classProgram { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, functionSource, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
        if (!classProgram.get() || error.isValid()) {
            failure = "U7: the codec fixture class failed to generate"_s;
            return false;
        }
        UnlinkedFunctionExecutable* method = nullptr;
        for (unsigned i = 0; i < classProgram->numberOfFunctionExprs(); ++i) {
            if (classProgram->functionExpr(i)->ecmaName().string() == "method"_s)
                method = classProgram->functionExpr(i);
        }
        if (!method || !method->parentScopeTDZVariables()) {
            failure = "U7: the codec fixture's method has no TDZ chain"_s;
            return false;
        }
        Strong<UnlinkedFunctionCodeBlock> methodBody { vm, method->unlinkedCodeBlockFor(vm, method->linkedSourceCode(functionSource), CodeSpecializationKind::CodeForCall, { }, error, method->parseMode()) };
        if (!methodBody.get() || error.isValid()) {
            failure = "U7: the codec fixture's method failed to generate"_s;
            return false;
        }
        RefPtr<CachedBytecode> methodCore = encodeUnlinkedCodeBlockCore(vm, *methodBody.get(), method, nullptr, encodeFailure);
        if (!methodCore) {
            failure = "U7: the codec fixture method's core failed to encode"_s;
            return false;
        }
        return runFunctionCases(vm, failure, *methodCore, *functionSource.provider(), *method);
    }
};

namespace JITCache {

// The part of U7 that alters a core's records (SPEC-ucb.codec.md, section 7), which runUCBSelfTest runs: it reports the
// first case that fails in `failure` and returns false.
bool runCoreCodecSelfTest(VM& vm, String& failure)
{
    return CoreCodecSelfTestAccess::run(vm, failure);
}

} // namespace JITCache

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
