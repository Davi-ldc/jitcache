#include "config.h"
#include "JITCacheSHA256.h"

#include <algorithm>
#include <bit>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

#if CPU(X86_64)
#include <cpuid.h>
#include <immintrin.h>
#elif CPU(ARM64) && OS(LINUX)
#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#if ENABLE(JITCACHE_TWINS)
#include <limits>
#include <wtf/HexNumber.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>
#endif

namespace JSC::JITCache {

// FIPS 180-4 SHA-256 (SPEC-ucb.md section 3.8). Every path compresses whole blocks; padding and buffering are shared, so
// the paths differ only in the compression function.
namespace SHA256Internal {

static constexpr size_t blockSize = 64;
static constexpr size_t lengthFieldSize = sizeof(uint64_t);

using HashWords = std::array<uint32_t, 8>;
using Block = std::array<uint8_t, blockSize>;

// FIPS 180-4 section 4.2.2.
alignas(16) static constexpr std::array<uint32_t, 64> roundConstants {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// FIPS 180-4 section 5.3.3.
static constexpr HashWords initialHashValue {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

// The functions of FIPS 180-4 section 4.1.2.
static constexpr uint32_t choose(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static constexpr uint32_t majority(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static constexpr uint32_t bigSigma0(uint32_t x)
{
    return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

static constexpr uint32_t bigSigma1(uint32_t x)
{
    return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

static constexpr uint32_t smallSigma0(uint32_t x)
{
    return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
}

static constexpr uint32_t smallSigma1(uint32_t x)
{
    return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
}

static uint32_t loadBigEndian32(std::span<const uint8_t, 4> bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

template<typename UnsignedInteger>
static void storeBigEndian(std::span<uint8_t, sizeof(UnsignedInteger)> bytes, UnsignedInteger value)
{
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8 * (bytes.size() - 1 - i)));
}

// Each compression function applies FIPS 180-4 section 6.2.2 to every block of `blocks`, whose size is a multiple of 64.
using Compressor = void (*)(HashWords&, std::span<const uint8_t> blocks);

// One round of FIPS 180-4 section 6.2.2, step 3. The working variables are renamed rather than moved: each round takes them
// rotated by one position from the round before, writes the new e into d and the new a into h, and eight rounds bring
// every name back to its variable.
static ALWAYS_INLINE void portableRound(uint32_t a, uint32_t b, uint32_t c, uint32_t& d, uint32_t e, uint32_t f, uint32_t g, uint32_t& h, uint32_t constantPlusWord)
{
    uint32_t t1 = h + bigSigma1(e) + choose(e, f, g) + constantPlusWord;
    d += t1;
    h = t1 + bigSigma0(a) + majority(a, b, c);
}

// The reference.
static void compressPortable(HashWords& state, std::span<const uint8_t> blocks)
{
    ASSERT(!(blocks.size() % blockSize));
    for (; !blocks.empty(); blocks = blocks.subspan(blockSize)) {
        std::array<uint32_t, 64> schedule;
        for (size_t t = 0; t < 16; ++t)
            schedule[t] = loadBigEndian32(blocks.subspan(4 * t).first<4>());
        for (size_t t = 16; t < 64; ++t)
            schedule[t] = smallSigma1(schedule[t - 2]) + schedule[t - 7] + smallSigma0(schedule[t - 15]) + schedule[t - 16];
        HashWords working = state;
        auto& [a, b, c, d, e, f, g, h] = working;
        for (size_t t = 0; t < 64; t += 8) {
            portableRound(a, b, c, d, e, f, g, h, roundConstants[t] + schedule[t]);
            portableRound(h, a, b, c, d, e, f, g, roundConstants[t + 1] + schedule[t + 1]);
            portableRound(g, h, a, b, c, d, e, f, roundConstants[t + 2] + schedule[t + 2]);
            portableRound(f, g, h, a, b, c, d, e, roundConstants[t + 3] + schedule[t + 3]);
            portableRound(e, f, g, h, a, b, c, d, roundConstants[t + 4] + schedule[t + 4]);
            portableRound(d, e, f, g, h, a, b, c, roundConstants[t + 5] + schedule[t + 5]);
            portableRound(c, d, e, f, g, h, a, b, roundConstants[t + 6] + schedule[t + 6]);
            portableRound(b, c, d, e, f, g, h, a, roundConstants[t + 7] + schedule[t + 7]);
        }
        for (size_t i = 0; i < state.size(); ++i)
            state[i] += working[i];
    }
}

#if CPU(X86_64)

// cpuid directly, as crc32c in runtime/CachedTypes.cpp does: __builtin_cpu_supports needs compiler-rt's __cpu_model, which
// not every link provides. The instructions use only XMM registers, whose state every x86_64 kernel saves.
static bool cpuHasSHAExtensions()
{
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & bit_SSSE3) || !(ecx & bit_SSE4_1))
        return false;
    return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & bit_SHA);
}

// A group is four rounds, 4 * group to 4 * group + 3, whose message words (FIPS 180-4 section 6.2.2, step 1) one register
// holds, the earliest round's in the low lane.
__attribute__((target("sha,sse4.1,ssse3"))) static ALWAYS_INLINE __m128i loadMessageWords(std::span<const uint8_t> block, size_t group)
{
    const __m128i byteSwapWords = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(block.subspan(16 * group, 16).data())), byteSwapWords);
}

__attribute__((target("sha,sse4.1,ssse3"))) static ALWAYS_INLINE __m128i nextMessageWords(__m128i fourGroupsBefore, __m128i threeGroupsBefore, __m128i twoGroupsBefore, __m128i groupBefore)
{
    __m128i partial = _mm_sha256msg1_epu32(fourGroupsBefore, threeGroupsBefore);
    partial = _mm_add_epi32(partial, _mm_alignr_epi8(groupBefore, twoGroupsBefore, 4));
    return _mm_sha256msg2_epu32(partial, groupBefore);
}

// The SHA extensions hold the hash words in two registers, ABEF and CDGH from the high lane to the low one. Each
// _mm_sha256rnds2_epu32 runs two rounds and returns the new ABEF, while the ABEF it was given becomes the new CDGH, so the
// two registers trade roles after the first call and trade back after the second.
__attribute__((target("sha,sse4.1,ssse3"))) static ALWAYS_INLINE void fourRounds(__m128i& abef, __m128i& cdgh, __m128i words, size_t group)
{
    __m128i wordsPlusConstants = _mm_add_epi32(words, _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(std::span { roundConstants }.subspan(4 * group, 4).data())));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wordsPlusConstants);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wordsPlusConstants, 0x0E));
}

__attribute__((target("sha,sse4.1,ssse3"))) static void compressWithSHAExtensions(HashWords& state, std::span<const uint8_t> blocks)
{
    ASSERT(!(blocks.size() % blockSize));
    __m128i cdab = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(state.data())), 0xB1);
    __m128i efgh = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(std::span { state }.subspan<4>().data())), 0x1B);
    __m128i abef = _mm_alignr_epi8(cdab, efgh, 8);
    __m128i cdgh = _mm_blend_epi16(efgh, cdab, 0xF0);
    for (; !blocks.empty(); blocks = blocks.subspan(blockSize)) {
        __m128i abefAtStart = abef;
        __m128i cdghAtStart = cdgh;
        // words0 to words3 hold the message words of four consecutive groups, each replaced by the group four later.
        __m128i words0 = loadMessageWords(blocks, 0);
        __m128i words1 = loadMessageWords(blocks, 1);
        __m128i words2 = loadMessageWords(blocks, 2);
        __m128i words3 = loadMessageWords(blocks, 3);
        fourRounds(abef, cdgh, words0, 0);
        fourRounds(abef, cdgh, words1, 1);
        fourRounds(abef, cdgh, words2, 2);
        fourRounds(abef, cdgh, words3, 3);
        for (size_t group = 4; group < 16; group += 4) {
            words0 = nextMessageWords(words0, words1, words2, words3);
            fourRounds(abef, cdgh, words0, group);
            words1 = nextMessageWords(words1, words2, words3, words0);
            fourRounds(abef, cdgh, words1, group + 1);
            words2 = nextMessageWords(words2, words3, words0, words1);
            fourRounds(abef, cdgh, words2, group + 2);
            words3 = nextMessageWords(words3, words0, words1, words2);
            fourRounds(abef, cdgh, words3, group + 3);
        }
        abef = _mm_add_epi32(abef, abefAtStart);
        cdgh = _mm_add_epi32(cdgh, cdghAtStart);
    }
    __m128i feba = _mm_shuffle_epi32(abef, 0x1B);
    __m128i dchg = _mm_shuffle_epi32(cdgh, 0xB1);
    _mm_storeu_si128(reinterpret_cast<__m128i_u*>(state.data()), _mm_blend_epi16(feba, dchg, 0xF0));
    _mm_storeu_si128(reinterpret_cast<__m128i_u*>(std::span { state }.subspan<4>().data()), _mm_alignr_epi8(dchg, feba, 8));
}

#elif CPU(ARM64) && OS(LINUX)

// The instructions use only the SIMD registers, whose state every ARM64 kernel saves.
static bool cpuHasSHA2Instructions()
{
    return getauxval(AT_HWCAP) & HWCAP_SHA2;
}

// A group is four rounds, 4 * group to 4 * group + 3, whose message words (FIPS 180-4 section 6.2.2, step 1) one register
// holds, the earliest round's in the low lane.
__attribute__((target("sha2"))) static ALWAYS_INLINE uint32x4_t loadMessageWords(std::span<const uint8_t> block, size_t group)
{
    return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block.subspan(16 * group, 16).data())));
}

__attribute__((target("sha2"))) static ALWAYS_INLINE uint32x4_t nextMessageWords(uint32x4_t fourGroupsBefore, uint32x4_t threeGroupsBefore, uint32x4_t twoGroupsBefore, uint32x4_t groupBefore)
{
    return vsha256su1q_u32(vsha256su0q_u32(fourGroupsBefore, threeGroupsBefore), twoGroupsBefore, groupBefore);
}

// The SHA-256 instructions hold the hash words as ABCD and EFGH, A in the low lane. vsha256hq_u32 returns the ABCD four
// rounds later and vsha256h2q_u32 the EFGH, which also reads the ABCD from before those rounds.
__attribute__((target("sha2"))) static ALWAYS_INLINE void fourRounds(uint32x4_t& abcd, uint32x4_t& efgh, uint32x4_t words, size_t group)
{
    uint32x4_t wordsPlusConstants = vaddq_u32(words, vld1q_u32(std::span { roundConstants }.subspan(4 * group, 4).data()));
    uint32x4_t abcdBefore = abcd;
    abcd = vsha256hq_u32(abcd, efgh, wordsPlusConstants);
    efgh = vsha256h2q_u32(efgh, abcdBefore, wordsPlusConstants);
}

__attribute__((target("sha2"))) static void compressWithSHA2Instructions(HashWords& state, std::span<const uint8_t> blocks)
{
    ASSERT(!(blocks.size() % blockSize));
    uint32x4_t abcd = vld1q_u32(state.data());
    uint32x4_t efgh = vld1q_u32(std::span { state }.subspan<4>().data());
    for (; !blocks.empty(); blocks = blocks.subspan(blockSize)) {
        uint32x4_t abcdAtStart = abcd;
        uint32x4_t efghAtStart = efgh;
        // words0 to words3 hold the message words of four consecutive groups, each replaced by the group four later.
        uint32x4_t words0 = loadMessageWords(blocks, 0);
        uint32x4_t words1 = loadMessageWords(blocks, 1);
        uint32x4_t words2 = loadMessageWords(blocks, 2);
        uint32x4_t words3 = loadMessageWords(blocks, 3);
        fourRounds(abcd, efgh, words0, 0);
        fourRounds(abcd, efgh, words1, 1);
        fourRounds(abcd, efgh, words2, 2);
        fourRounds(abcd, efgh, words3, 3);
        for (size_t group = 4; group < 16; group += 4) {
            words0 = nextMessageWords(words0, words1, words2, words3);
            fourRounds(abcd, efgh, words0, group);
            words1 = nextMessageWords(words1, words2, words3, words0);
            fourRounds(abcd, efgh, words1, group + 1);
            words2 = nextMessageWords(words2, words3, words0, words1);
            fourRounds(abcd, efgh, words2, group + 2);
            words3 = nextMessageWords(words3, words0, words1, words2);
            fourRounds(abcd, efgh, words3, group + 3);
        }
        abcd = vaddq_u32(abcd, abcdAtStart);
        efgh = vaddq_u32(efgh, efghAtStart);
    }
    vst1q_u32(state.data(), abcd);
    vst1q_u32(std::span { state }.subspan<4>().data(), efgh);
}

#endif

struct Path {
    ASCIILiteral name;
    Compressor compress;
    bool (*isOffered)();
};

static bool isAlwaysOffered()
{
    return true;
}

// The portable reference first, then the accelerated path this build has for its CPU family.
static constexpr std::array paths {
    Path { "the portable path"_s, compressPortable, isAlwaysOffered },
#if CPU(X86_64)
    Path { "the SHA extensions path"_s, compressWithSHAExtensions, cpuHasSHAExtensions },
#elif CPU(ARM64) && OS(LINUX)
    Path { "the SHA-256 instructions path"_s, compressWithSHA2Instructions, cpuHasSHA2Instructions },
#endif
};

// The process runs the last path its CPU offers, chosen once.
static Compressor processCompressor()
{
    static const Compressor compressor = [] {
        Compressor chosen = compressPortable;
        for (auto& path : paths) {
            if (path.isOffered())
                chosen = path.compress;
        }
        return chosen;
    }();
    return compressor;
}

// Buffers a partial block and compresses every whole one; the caller's stream holds bufferLength < 64 bytes in buffer.
static void absorb(Compressor compress, HashWords& state, Block& buffer, size_t& bufferLength, uint64_t& messageLength, std::span<const uint8_t> data)
{
    ASSERT(bufferLength < blockSize);
    if (data.empty())
        return;
    messageLength += data.size();
    if (bufferLength) {
        size_t taken = std::min(data.size(), blockSize - bufferLength);
        memcpySpan(std::span { buffer }.subspan(bufferLength), data.first(taken));
        bufferLength += taken;
        data = data.subspan(taken);
        if (bufferLength < blockSize)
            return;
        compress(state, buffer);
        bufferLength = 0;
    }
    size_t wholeBlocksSize = data.size() - data.size() % blockSize;
    if (wholeBlocksSize) {
        compress(state, data.first(wholeBlocksSize));
        data = data.subspan(wholeBlocksSize);
    }
    if (data.empty())
        return;
    memcpySpan(std::span { buffer }, data);
    bufferLength = data.size();
}

// FIPS 180-4 section 5.1.1: a one bit, zeros up to 56 bytes modulo 64, then the message length in bits as a big-endian
// u64; the digest is the eight hash words, big-endian (section 6.2.2, last step).
static Digest256 finish(Compressor compress, HashWords& state, Block& buffer, size_t& bufferLength, uint64_t messageLength)
{
    ASSERT(bufferLength < blockSize);
    buffer[bufferLength++] = 0x80;
    if (bufferLength > blockSize - lengthFieldSize) {
        zeroSpan(std::span { buffer }.subspan(bufferLength));
        compress(state, buffer);
        bufferLength = 0;
    }
    zeroSpan(std::span { buffer }.subspan(bufferLength, blockSize - lengthFieldSize - bufferLength));
    storeBigEndian(std::span { buffer }.last<lengthFieldSize>(), messageLength * 8);
    compress(state, buffer);
    bufferLength = 0;
    Digest256 digest;
    for (size_t i = 0; i < state.size(); ++i)
        storeBigEndian(std::span { digest }.subspan(4 * i).first<4>(), state[i]);
    return digest;
}

} // namespace SHA256Internal

SHA256::SHA256()
    : m_state(SHA256Internal::initialHashValue)
    , m_buffer()
    , m_bufferLength(0)
    , m_messageLength(0)
{
}

void SHA256::update(std::span<const uint8_t> data)
{
    SHA256Internal::absorb(SHA256Internal::processCompressor(), m_state, m_buffer, m_bufferLength, m_messageLength, data);
}

Digest256 SHA256::finalize()
{
    return SHA256Internal::finish(SHA256Internal::processCompressor(), m_state, m_buffer, m_bufferLength, m_messageLength);
}

Digest256 SHA256::hash(std::span<const uint8_t> data)
{
    SHA256 hasher;
    hasher.update(data);
    return hasher.finalize();
}

#if ENABLE(JITCACHE_TWINS)

namespace SHA256Internal {

static constexpr size_t wholeMessage = std::numeric_limits<size_t>::max();

// Feeds the message to a fresh stream on the given path in updates of at most chunkSize bytes.
static Digest256 digestOnPath(Compressor compress, std::span<const uint8_t> message, size_t chunkSize)
{
    HashWords state = initialHashValue;
    Block buffer { };
    size_t bufferLength = 0;
    uint64_t messageLength = 0;
    while (!message.empty()) {
        auto piece = message.first(std::min(chunkSize, message.size()));
        absorb(compress, state, buffer, bufferLength, messageLength, piece);
        message = message.subspan(piece.size());
    }
    return finish(compress, state, buffer, bufferLength, messageLength);
}

// The same through the class, which runs on the path the process chose.
static Digest256 digestWithClass(std::span<const uint8_t> message, size_t chunkSize)
{
    SHA256 hasher;
    while (!message.empty()) {
        auto piece = message.first(std::min(chunkSize, message.size()));
        hasher.update(piece);
        message = message.subspan(piece.size());
    }
    return hasher.finalize();
}

static String feeding(size_t chunkSize)
{
    if (chunkSize == wholeMessage)
        return "in one update"_s;
    return makeString("in "_s, chunkSize, "-byte updates"_s);
}

struct Example {
    ASCIILiteral name;
    std::span<const uint8_t> message;
    ASCIILiteral digest; // uppercase hexadecimal, as toHexString writes it
};

// The FIPS 180-4 examples on every path the CPU offers and through the class, each whole and in updates that split it
// across blocks.
static bool checkExamples(String& failure)
{
    Vector<uint8_t> millionA(FillWith { }, 1000000, 'a');
    const std::array examples {
        Example { "the empty message"_s, { }, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"_s },
        Example { "\"abc\""_s, byteCast<uint8_t>("abc"_span), "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"_s },
        Example { "the 448-bit message"_s, byteCast<uint8_t>("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"_span), "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1"_s },
        Example { "the 896-bit message"_s, byteCast<uint8_t>("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"_span), "CF5B16A778AF8380036CE59E7B0492370B249B11E8F07A51AFAC45037AFEE9D1"_s },
        Example { "one million \"a\""_s, millionA.span(), "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0"_s },
    };
    static constexpr std::array<size_t, 4> chunkSizes { wholeMessage, 1, 63, 1000 };
    // Updates shorter than a block exercise the buffering, which the short examples already do byte by byte.
    static constexpr size_t longestMessageFedInShortUpdates = 4096;

    auto check = [&](ASCIILiteral what, const Example& example, size_t chunkSize, const Digest256& digest) {
        String actual = toHexString(digest);
        if (actual == example.digest)
            return true;
        failure = makeString("U1: "_s, what, " gives "_s, actual, " for "_s, example.name, ' ', feeding(chunkSize), ", not "_s, example.digest);
        return false;
    };
    for (auto& example : examples) {
        for (size_t chunkSize : chunkSizes) {
            if (chunkSize < blockSize && example.message.size() > longestMessageFedInShortUpdates)
                continue;
            for (auto& path : paths) {
                if (path.isOffered() && !check(path.name, example, chunkSize, digestOnPath(path.compress, example.message, chunkSize)))
                    return false;
            }
            if (!check("the SHA256 class"_s, example, chunkSize, digestWithClass(example.message, chunkSize)))
                return false;
        }
        if (!check("SHA256::hash"_s, example, wholeMessage, SHA256::hash(example.message)))
            return false;
    }
    return true;
}

// 4096 random messages of random lengths up to four blocks, each digested on every path the CPU offers, whole and in random
// updates, and through the class, all compared with the portable path's digest of the whole message. Short messages hit
// each padding case often; the million-byte example covers long runs of whole blocks.
static bool compareOverRandomLengths(String& failure)
{
    static constexpr unsigned lengthCount = 4096;
    static constexpr unsigned maximumLength = 4 * blockSize;
    static constexpr unsigned maximumChunkSize = 2 * blockSize;
    WeakRandom random;
    Vector<uint8_t> noise(2 * maximumLength);
    for (auto& byte : noise)
        byte = static_cast<uint8_t>(random.getUint32());

    for (unsigned i = 0; i < lengthCount; ++i) {
        size_t length = random.getUint32(maximumLength + 1);
        size_t offset = random.getUint32(maximumLength + 1);
        size_t chunkSize = 1 + random.getUint32(maximumChunkSize);
        auto message = noise.span().subspan(offset, length);
        Digest256 reference = digestOnPath(compressPortable, message, wholeMessage);
        auto check = [&](ASCIILiteral what, size_t feedSize, const Digest256& digest) {
            if (digest == reference)
                return true;
            failure = makeString("U1: "_s, what, " differs from the portable path on a random message of "_s, length, " bytes "_s, feeding(feedSize), " (seed "_s, random.seed(), ')');
            return false;
        };
        for (auto& path : paths) {
            if (!path.isOffered())
                continue;
            // The portable path's whole digest is the reference itself.
            if (path.compress != compressPortable && !check(path.name, wholeMessage, digestOnPath(path.compress, message, wholeMessage)))
                return false;
            if (!check(path.name, chunkSize, digestOnPath(path.compress, message, chunkSize)))
                return false;
        }
        if (!check("the SHA256 class"_s, chunkSize, digestWithClass(message, chunkSize)) || !check("SHA256::hash"_s, wholeMessage, SHA256::hash(message)))
            return false;
    }
    return true;
}

} // namespace SHA256Internal

// Self-test U1 (SPEC-ucb.md section 13.1), which runUCBSelfTest runs. It lives beside the paths because they are private to
// this file; it reports the first difference in `failure` and returns false.
bool runSHA256SelfTest(String& failure)
{
    return SHA256Internal::checkExamples(failure) && SHA256Internal::compareOverRandomLengths(failure);
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC::JITCache
