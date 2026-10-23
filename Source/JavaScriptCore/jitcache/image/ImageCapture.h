#pragma once

#if ENABLE(JIT)

#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageTypes.h"
#include <expected>
#include <stddef.h>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/Vector.h>

// The capture of a recorded image: what the capture glue writes as image.baseline and baked-facts.baseline for an
// eligible CB, and in twins builds image-twins.baseline. captureImage reads the BaselineJITCode, its record, its MathICs,
// its executable memory up to the linked sizes and the UCB, keeps only what the writes cannot read again (each mold's
// identifier field, and in twins builds the producer values), and the write functions stream the sections from the live
// code and tables with no buffer of their own.

namespace JSC {

class BaselineJITCode;
class CodeBlock;
class VM;

namespace JITCache {

class ProducerBudget;

enum class CaptureOutcome : uint8_t {
    // Not a fault: the record is not Complete, or capture found what makes it Unrecordable (a mold identifier that is
    // neither a UCB identifier nor an immortal name, a MathIC snippet without matching provenance). The glue treats the
    // CB as ineligible.
    NotEligible,
    // The budget refused a charge and has raised the recording fault itself; the capture stopped.
    ChargeRefused,
};

// Whether the code carries a record in state Complete, which a record reaches only for shareable code. The glue checks
// the other conditions of a body's eligibility itself.
bool isImageCapturable(const BaselineJITCode&);

// The sections of one capture. Everything it keeps is charged to the producer budget, and destroying it releases exactly
// that charge. It holds a reference to the code it describes; the writes must run before JS resumes, since only JS
// regenerates a MathIC or rewrites the coverage rates.
class ImageCapture {
    WTF_MAKE_NONCOPYABLE(ImageCapture);
public:
    ImageCapture(ImageCapture&&);
    ~ImageCapture();

    size_t imageSectionSize() const;
    size_t bakedFactsSectionSize() const;
    // Each streams its section through the sink and returns false once the sink refuses.
    [[nodiscard]] bool writeImageSection(const ImageSectionSink&) const;
    [[nodiscard]] bool writeBakedFactsSection(const ImageSectionSink&) const;
#if ENABLE(JITCACHE_TWINS)
    // The image-twins.baseline section: the record's twin data, the producer values, and this process's
    // captureProcessToken(), streamed through the section's writer in ImageTwinsSection.h.
    size_t twinsSectionSize() const;
    [[nodiscard]] bool writeTwinsSection(const ImageSectionSink&) const;
#endif

private:
    friend std::expected<ImageCapture, CaptureOutcome> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&);

    // A mold's identifier as the molds region encodes it: an index into the UCB's identifiers, an ImmortalName, or none.
    struct MoldIdentifier {
        MoldIdentifierKind kind { MoldIdentifierKind::None };
        uint32_t value { 0 };
    };

    // Takes over chargedBytes, the bytes of moldIdentifiers' storage and in twins builds of producerValues'.
#if ENABLE(JITCACHE_TWINS)
    ImageCapture(BaselineJITCode&, ProducerBudget&, size_t chargedBytes, const ImageSectionHeader&, Vector<MoldIdentifier>&&, Vector<uint64_t>&& producerValues);
#else
    ImageCapture(BaselineJITCode&, ProducerBudget&, size_t chargedBytes, const ImageSectionHeader&, Vector<MoldIdentifier>&&);
#endif

    // The offset of a code pointer of the side tables or a MathIC location in the image, which JIT::link placed inside
    // [start, start + codeSize] (debug builds ASSERT).
    uint32_t imageOffsetOf(const void* location) const;
    // A MathIC's entry in the MathICs region, from its live IC when it has inline code and from the record alone otherwise.
    ImageMathICEntry mathICEntryOf(const MathICRecord&) const;

    Ref<BaselineJITCode> m_code;
    Ref<ProducerBudget> m_budget;
    size_t m_chargedBytes { 0 };
    const void* m_imageStart { nullptr }; // the image's normal entry, offset 0
    ImageSectionHeader m_header; // the counts and fields of the section header, read at capture
    Vector<MoldIdentifier> m_moldIdentifiers; // in mold order
#if ENABLE(JITCACHE_TWINS)
    // What each fixup's target resolved to in the capturing VM: the image's in footprint order, then each snippet's in
    // MathIC index order. The twin check compares them with its own resolutions to find addresses that did not move.
    Vector<uint64_t> m_producerValues;
#endif
};

// Captures the image, on the VM thread with the API lock and heap access, JS paused and no collector phase, on a
// BaselineJITCode the caller holds a reference to. It allocates no cell and drains nothing. Its only JSC lock is
// JITThunks::m_lock, which twins builds take in the native support lookups that resolve every fixup for the producer
// values; every target they resolve already exists. Every byte it allocates is charged to the budget first. When it
// meets what makes a record Unrecordable (an unknown mold identifier, a MathIC snippet without matching provenance), it
// marks the record and returns NotEligible. It trusts that JIT::link and the MathIC hooks left the side tables and the
// MathICs as the record says.
std::expected<ImageCapture, CaptureOutcome> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
