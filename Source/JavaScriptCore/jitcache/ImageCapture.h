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

// The capture of a recorded image (SPEC-image.md section 9): what the capture glue writes as image.baseline and
// baked-facts.baseline for an eligible CB. captureImage reads the BaselineJITCode, its record, its MathICs, its executable
// memory up to the linked sizes and the UCB, keeps only what the writes cannot read again (each mold's identifier field),
// and the write functions stream the sections from the live code and tables with no buffer of their own.

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
    // The budget refused a charge and has raised the recording fault itself (R-INT-2); the capture stopped.
    ChargeRefused,
    // A strict check, S1 to S4, rejected what the capture read from the producer's own state.
    RecordingFault,
};

struct CaptureFailure {
    CaptureOutcome outcome;
    ImageCheck check; // S1 to S4 for RecordingFault, else None
};

// Whether the code carries a record in state Complete, which a record reaches only for shareable code (section 4.7,
// step 4). The glue checks THREAD's other eligibility conditions itself.
bool isImageCapturable(const BaselineJITCode&);

// The sections of one capture. Everything it keeps is charged to the producer budget, and destroying it releases exactly
// that charge (I14). It holds a reference to the code it describes; the writes must run before JS resumes, since only JS
// regenerates a MathIC or rewrites the coverage rates.
class ImageCapture {
    WTF_MAKE_NONCOPYABLE(ImageCapture);
public:
    ImageCapture(ImageCapture&&);
    ~ImageCapture();

    size_t imageSectionSize() const;
    size_t bakedFactsSectionSize() const;
    // Each streams its section through the sink, as section 8 lays it out, and returns false once the sink refuses.
    [[nodiscard]] bool writeImageSection(const ImageSectionSink&) const;
    [[nodiscard]] bool writeBakedFactsSection(const ImageSectionSink&) const;
#if ENABLE(JITCACHE_TWINS)
    // The image-twins.baseline section (section 11.2): the record's twin data, the producer values of section 9, step 6,
    // and this process's captureProcessToken(). Defined with that section's codec.
    size_t twinsSectionSize() const;
    [[nodiscard]] bool writeTwinsSection(const ImageSectionSink&) const;
#endif

private:
    friend std::expected<ImageCapture, CaptureFailure> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&, bool strict);

    // A mold's identifier as region 4 encodes it: an index into the UCB's identifiers, an ImmortalName, or none.
    struct MoldIdentifier {
        MoldIdentifierKind kind { MoldIdentifierKind::None };
        uint32_t value { 0 };
    };

    // Takes over chargedBytes, the bytes of moldIdentifiers' storage.
    ImageCapture(BaselineJITCode&, ProducerBudget&, size_t chargedBytes, const ImageSectionHeader&, Vector<MoldIdentifier>&&);

    // The offset of a code pointer of the side tables or a MathIC location in the image, which lies inside
    // [start, start + codeSize]: S4 checked it under strict, and JIT::link placed it there otherwise (debug builds ASSERT).
    uint32_t imageOffsetOf(const void* location) const;
    // A MathIC's region 9 entry, from its live IC when it has inline code and from the record alone otherwise.
    ImageMathICEntry mathICEntryOf(const MathICRecord&) const;

    Ref<BaselineJITCode> m_code;
    Ref<ProducerBudget> m_budget;
    size_t m_chargedBytes { 0 };
    const void* m_imageStart { nullptr }; // the image's normal entry, offset 0
    ImageSectionHeader m_header; // the counts and fields of the section header, read at capture
    Vector<MoldIdentifier> m_moldIdentifiers; // in mold order
};

// The capture of section 9, on the VM thread with the API lock and heap access, JS paused and no collector phase, on a
// BaselineJITCode the caller holds a reference to (R-INT-6). It allocates no cell, drains nothing and takes no JSC lock
// other than JITThunks::m_lock, which the native support lookups of strict's S1 and S2 take; every target they resolve
// already exists. Every byte it allocates is charged to the budget first. When it meets a reason of section 4.1 (an
// unknown mold identifier, a MathIC snippet without matching provenance), it marks the record Unrecordable and returns
// NotEligible; otherwise, under strict, it runs S1 to S4.
std::expected<ImageCapture, CaptureFailure> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&, bool strict);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
