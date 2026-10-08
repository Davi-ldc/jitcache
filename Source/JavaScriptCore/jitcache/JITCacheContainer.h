/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <bit>
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

// Every integer of the artifact is little-endian, as every supported target is, and is read with memcpy-based loads
// (container sub-SPEC).
static_assert(std::endian::native == std::endian::little);

namespace JSC::JITCache {

// Body validation (container sub-SPEC section 4.5). Each check has an integrity part, which both modes run, and some
// have a structure part, which only Full adds. The checks run in this order, and validation names the first that fails:
//
//   B1 container.size           integrity: the file is at least 128 bytes and its size equals the envelope's file size
//   B2 container.envelope       integrity: the tag, layout version 1, envelope size 128, N from 1 to 64, the envelope's
//                               CRC; structure: the zero bytes and a highest tier of 1
//   B3 container.key            integrity: the envelope's key equals the key the file's name encodes
//   B4 container.header-digest  integrity: the envelope's header digest equals the opened artifact's
//   B5 container.version        structure: the commit identifier is not zero
//   B6 container.directory      integrity: the directory's CRC; every entry's offset and size lie inside the file
//                               without overflow; structure: every entry's zero byte, every (type id, tier) a section
//                               kind, entries strictly increasing, offsets and sizes as section 4.3 lays them out, zero
//                               padding
//   B7 container.required       structure: every section required for the highest tier is present, and
//                               image-twins.baseline exactly in ENABLE(JITCACHE_TWINS) builds
//   B8 container.checksum       integrity: each section's CRC
enum class ValidationMode : uint8_t { Integrity, Full };

using ContainerCheck = ASCIILiteral; // the failing check's name in the table above, such as "container.key"

struct SectionExtent {
    uint64_t offset;
    uint64_t size;
};
struct BodyLayout {
    uint64_t version; // the commit identifier
    uint8_t highestTier;
    uint32_t llintThreshold; // L
    uint32_t counterProgress; // P
    std::array<std::optional<SectionExtent>, numberOfSectionKinds> sections; // by SectionKind; nullopt when absent
};

// With Integrity, the layout records the first directory entry of each section kind and ignores an entry whose
// (type id, tier) names no kind; the writer produces neither case, and normal mode trusts it not to (section 7.4).
std::expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode);

// The same checks, in the same order, over the file's bytes fed in order in chunks of any size down to one byte: the
// writer's reread (section 8.2, step 6) feeds it through the staging buffer, and validateBody feeds its whole span to one
// stream. It keeps the envelope and directory (at most 128 + 64 * 24 bytes) and a CRC per section; allocates nothing.
class BodyValidationStream {
    WTF_MAKE_NONCOPYABLE(BodyValidationStream);
public:
    BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode, uint64_t fileSize);
    void append(std::span<const uint8_t>); // the file's bytes in order
    std::expected<BodyLayout, ContainerCheck> finish(); // after the last byte; names the first check that failed

private:
    static constexpr size_t envelopeBytes = 128;
    static constexpr size_t directoryEntryBytes = 24;
    static constexpr unsigned maximumSections = 64;
    static constexpr size_t maximumPrefixBytes = envelopeBytes + maximumSections * directoryEntryBytes;

    const BodyKey m_expectedKey;
    const std::array<uint8_t, 16> m_headerDigest;
    const ValidationMode m_mode;
    const uint64_t m_fileSize;
    uint64_t m_received { 0 }; // the bytes appended so far
    unsigned m_sectionCount { 0 }; // N, once the envelope has arrived with an N from 1 to 64; 0 before
    bool m_directoryRead { false }; // the whole directory has arrived and its entries are known
    bool m_paddingIsZero { true }; // every byte past the directory that no section covers was zero (B6, Full)
    std::array<uint8_t, maximumPrefixBytes> m_prefix { }; // the envelope and the directory, as they arrived
    std::array<uint32_t, maximumSections> m_sectionCRCStates { }; // each directory entry's running crc32cExtend state
};

} // namespace JSC::JITCache
