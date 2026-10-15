#pragma once

#if ENABLE(JIT)

#include "ImageSection.h"
#include "ImageTypes.h"
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
    // A strict check, ImageCheck::S1 to S4, rejected what the capture read from the producer's own state.
    RecordingFault,
};

struct CaptureFailure {
    CaptureOutcome outcome;
    ImageCheck check; // ImageCheck::S1 to S4 for RecordingFault, else None
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
    friend std::expected<ImageCapture, CaptureFailure> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&, bool strict);

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

    // The offset of a code pointer of the side tables or a MathIC location in the image, which lies inside
    // [start, start + codeSize]: ImageCheck::S4 checked it under strict, and JIT::link placed it there otherwise (debug
    // builds ASSERT).
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
// BaselineJITCode the caller holds a reference to. It allocates no cell, drains nothing and takes no JSC lock other than
// JITThunks::m_lock, which the native support lookups of strict's ImageCheck::S1 and S2 take; every target they resolve
// already exists. Every byte it allocates is charged to the budget first. When it meets what makes a record
// Unrecordable (an unknown mold identifier, a MathIC snippet without matching provenance), it marks the record and
// returns NotEligible; otherwise, under strict, it runs ImageCheck::S1 to S4. In twins builds it first resolves every
// fixup for the producer values, and the ChangeRecordedTarget test hook changes the record's first fixup before S1 runs.
std::expected<ImageCapture, CaptureFailure> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&, bool strict);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
