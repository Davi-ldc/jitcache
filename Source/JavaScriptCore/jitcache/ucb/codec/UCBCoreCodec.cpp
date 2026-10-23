// The core codec's entry points, which CachedTypes.h declares. runtime/CachedTypes.cpp includes this file at its end,
// after the Encoder, the Decoder and the record types the entry points drive, which are local to that file. It compiles
// only there, so no Sources.txt lists it.

namespace JSC {

// A core and a descriptor start with a 16-byte header: a magic at offset 0, a core's kind at offset 4 (a descriptor's
// is 0), rootOffset at offset 8, the offset of the root record from the payload's start, and zero bytes elsewhere.
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

// The start chain: for a function core, the TDZ chain of the UFE whose slot holds the UCB; none otherwise.
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

// A core: the header, the root record of the UCB's class, its rootOffset, then the deferred work.
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

// A descriptor: the UFE's record as a core writes a child, alone, with its own chain as the start chain, so that the chain
// is one start record.
static void coreCodecEncodeDescriptor(Encoder& encoder, const UnlinkedFunctionExecutable& executable)
{
    auto header = encoder.malloc(coreCodecHeaderSize, encoderMaxAlignment);
    CachedFunctionExecutable* record = encoder.mallocFor<CachedFunctionExecutable>(executable);
    record->encode(encoder, executable);
    coreCodecWriteHeader(header, coreCodecDescriptorMagic, 0, encoder.offsetOf(record));
    encoder.encodeDeferred();
}

// One JITCacheCore encode and its charges. The encoder is freed before every charge it made is released; a payload it
// releases stays charged for its caller.
class CoreCodecEncode {
    WTF_MAKE_NONCOPYABLE(CoreCodecEncode);
    WTF_FORBID_HEAP_ALLOCATION;

public:
    CoreCodecEncode(VM& vm, CoreEncodingBudget* budget, RefPtr<TDZEnvironmentLink>&& startChain)
        : m_charges { budget }
        , m_startChain(WTF::move(startChain))
        , m_encoder(vm, m_fileHandle, Encoder::NumberStrings::Yes, nullptr, BytecodeCacheUpdatable::No, Encoder::Purpose::JITCacheCore, budget, m_startChain.get())
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

    // The payload's pages in order, unless anything was refused, in which case the sink sees nothing.
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

CoreEncodeFailure forEachUnlinkedFunctionExecutableDescriptorChunk(VM& vm, const UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, const ScopedLambda<void(std::span<const uint8_t>)>& sink)
{
    CoreCodecEncode encode(vm, budget, executable.parentScopeTDZVariables());
    coreCodecEncodeDescriptor(encode.encoder(), executable);
    encode.forEachPage(sink);
    return encode.failure();
}

// The root record of the requested kind, decoded; null, after recording Malformed, when the root offset does not place a
// whole record, aligned, inside the payload.
template<typename Record>
static UnlinkedCodeBlock* coreCodecDecodeRoot(Decoder& decoder, std::span<const uint8_t> payload, uint32_t rootOffset)
{
    if (rootOffset % alignof(Record) || rootOffset > payload.size() || payload.size() - rootOffset < sizeof(Record)) {
        decoder.noteFailure(CoreDecodeFailure::Malformed);
        return nullptr;
    }
    const Record* record = std::bit_cast<const Record*>(payload.data() + rootOffset);
    return record->decode(decoder);
}

UnlinkedCodeBlock* decodeUnlinkedCodeBlockCore(VM& vm, Ref<CachedBytecode> cachedBytecode, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, CoreDecodeFailure& failure)
{
    RELEASE_ASSERT(source.provider());
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

    // The payload stays alive in the decoder, which registers no persistent payload; no UFE keeps the decoder, since a
    // core writes no child body and its decode defers nothing.
    Ref decoder = Decoder::create(vm, WTF::move(cachedBytecode), source.provider(), Decoder::RecoverableCode::No, Decoder::Purpose::JITCacheCore, coreCodecStartChain(kind, holder));
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
    // A root that decodes as null has recorded its failure. Any failure discards the UCB, which nothing references; the
    // collector frees it.
    if (!codeBlock && decoder->failure() == CoreDecodeFailure::None)
        decoder->noteFailure(CoreDecodeFailure::Malformed);
    failure = decoder->failure();
    if (failure != CoreDecodeFailure::None)
        return nullptr;
    return codeBlock;
}

} // namespace JSC
