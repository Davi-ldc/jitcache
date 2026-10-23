// The core codec's self-test of altered records, for test builds. runtime/CachedTypes.cpp includes this file at its end,
// after the record types whose private members the cases change through the friend declarations those types hold. It
// compiles only there, so no Sources.txt lists it.

#if ENABLE(JITCACHE_TWINS)

#include "JSLock.h"
#include "ParserError.h"
#include "jitcache/ucb/UCBTwinGeneration.h"
#include <wtf/text/MakeString.h>

namespace JSC {

// Each case encodes a core, changes one record of a copy through the record's type, and decodes the copy, which must
// fail with the failure the case expects and not crash.
struct CoreCodecSelfTestAccess {
    // A program whose core holds the records the cases alter: more than nine global vars, so the var declarations take
    // hashed storage, and a property name long enough to have a string record of its own.
    static constexpr ASCIILiteral programText =
        "var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11;\n"
        "globalThis.propertyName = v0;\n"_s;

    // A method created while its class's binding is under TDZ, so that its UFE holds a TDZ chain, and an arrow created
    // while the method's own bindings are under TDZ, whose chain is a link naming them over the method's chain: the
    // method's core holds that link, its environment and a start record.
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

    // The string record of the identifier `name`; null when there is none or its string is inline.
    template<typename Record>
    static CachedUniquedStringImpl* identifierRecord(Record& record, ASCIILiteral name)
    {
        for (auto& identifier : identifiers(record)) {
            auto* string = stringRecord(identifier.m_string.m_impl.ptr());
            if (string && hasCharacters(*string, name))
                return string;
        }
        return nullptr;
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
    static bool expect(VM& vm, String& failure, ASCIILiteral name, const CachedBytecode& core, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, CoreDecodeFailure expected, const Alter& alter)
    {
        auto copy = MallocSpan<uint8_t, VMMalloc>::malloc(core.size());
        memcpySpan(copy.mutableSpan(), core.span());
        if (!alter(copy.mutableSpan())) {
            failure = makeString("the codec fixture has no record for the case "_s, name);
            return false;
        }
        CoreDecodeFailure result = CoreDecodeFailure::None;
        UnlinkedCodeBlock* decoded = decodeUnlinkedCodeBlockCore(vm, CachedBytecode::create(WTF::move(copy), { }), source, kind, holder, result);
        if (result == expected && (expected == CoreDecodeFailure::None) == !!decoded)
            return true;
        failure = makeString(name, " decoded to "_s, failureName(result), " instead of "_s, failureName(expected));
        return false;
    }

    static bool runProgramCases(VM& vm, String& failure, const CachedBytecode& core, const SourceCode& source)
    {
        using Program = CachedProgramCodeBlock;
        constexpr auto kind = UnlinkedCodeBlockCoreKind::Program;
        auto unaltered = [](std::span<uint8_t>) {
            return true;
        };
        auto hashedMap = [](Program& record) -> auto& { return record.m_varDeclarations.m_map; };

        if (!expect(vm, failure, "the unaltered program core"_s, core, source, kind, nullptr, CoreDecodeFailure::None, unaltered))
            return false;

        // Hashed map layouts.
        if (!expect(vm, failure, "a hashed declaration map with a deleted bucket"_s, core, source, kind, nullptr, CoreDecodeFailure::InconsistentMapLayout, [&](std::span<uint8_t> payload) {
            auto& map = hashedMap(root<Program>(payload));
            auto slots = elements(map.m_slots);
            if (!map.m_hashedCapacity || slots.empty())
                return false;
            slots[0] |= 1;
            return true;
        }))
            return false;
        if (!expect(vm, failure, "a hashed declaration map whose capacity exceeds minLoadInverse times its entries"_s, core, source, kind, nullptr, CoreDecodeFailure::InconsistentMapLayout, [&](std::span<uint8_t> payload) {
            auto& map = hashedMap(root<Program>(payload));
            if (!map.m_hashedCapacity)
                return false;
            map.m_hashedCapacity = 1u << 30;
            return true;
        }))
            return false;

        // Unresolved symbols: a private name this VM lacks.
        return expect(vm, failure, "a payload naming an absent private name"_s, core, source, kind, nullptr, CoreDecodeFailure::UnresolvedSymbol, [&](std::span<uint8_t> payload) {
            auto* record = identifierRecord(root<Program>(payload), "propertyName"_s);
            if (!record)
                return false;
            record->m_isSymbol = 1;
            record->m_isPrivate = 1;
            record->m_isRegistered = 0;
            record->m_isWellKnownSymbol = 0;
            record->m_ordinal = CachedUniquedStringImpl::noOrdinal;
            return true;
        });
    }

    static bool runFunctionCases(VM& vm, String& failure, const CachedBytecode& core, const SourceCode& source, const UnlinkedFunctionExecutable& holder)
    {
        constexpr auto kind = UnlinkedCodeBlockCoreKind::Function;
        auto unaltered = [](std::span<uint8_t>) {
            return true;
        };

        if (!expect(vm, failure, "the unaltered function core"_s, core, source, kind, &holder, CoreDecodeFailure::None, unaltered))
            return false;

        // Start records.
        if (!expect(vm, failure, "a start record decoded without a start chain"_s, core, source, kind, nullptr, CoreDecodeFailure::Malformed, [&](std::span<uint8_t> payload) {
            return !!startRecord(payload);
        }))
            return false;
        return expect(vm, failure, "a start record with an empty handle and a nonempty parent"_s, core, source, kind, &holder, CoreDecodeFailure::Malformed, [&](std::span<uint8_t> payload) {
            auto* link = startRecord(payload);
            if (!link)
                return false;
            // The parent names the record itself, which a decode that followed it would loop on.
            pointAt(link->m_parent.ptr(), link);
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
            failure = "the codec fixture program failed to generate"_s;
            return false;
        }
        CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> programCore = encodeUnlinkedCodeBlockCore(vm, *program.get(), nullptr, nullptr, encodeFailure);
        if (!programCore) {
            failure = "the codec fixture program's core failed to encode"_s;
            return false;
        }
        if (!runProgramCases(vm, failure, *programCore, programSource))
            return false;

        SourceCode functionSource = makeSource(String { functionText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<UnlinkedProgramCodeBlock> classProgram { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, functionSource, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
        if (!classProgram.get() || error.isValid()) {
            failure = "the codec fixture class failed to generate"_s;
            return false;
        }
        UnlinkedFunctionExecutable* method = nullptr;
        for (unsigned i = 0; i < classProgram->numberOfFunctionExprs(); ++i) {
            if (classProgram->functionExpr(i)->ecmaName().string() == "method"_s)
                method = classProgram->functionExpr(i);
        }
        if (!method || !method->parentScopeTDZVariables()) {
            failure = "the codec fixture's method has no TDZ chain"_s;
            return false;
        }
        // The method's body is generated from its own source, and its core is decoded against that source, as an import
        // decodes against the request's source.
        SourceCode methodSource = method->linkedSourceCode(functionSource);
        Strong<UnlinkedFunctionCodeBlock> methodBody { vm, method->unlinkedCodeBlockFor(vm, methodSource, CodeSpecializationKind::CodeForCall, { }, error, method->parseMode()) };
        if (!methodBody.get() || error.isValid()) {
            failure = "the codec fixture's method failed to generate"_s;
            return false;
        }
        RefPtr<CachedBytecode> methodCore = encodeUnlinkedCodeBlockCore(vm, *methodBody.get(), method, nullptr, encodeFailure);
        if (!methodCore) {
            failure = "the codec fixture method's core failed to encode"_s;
            return false;
        }
        return runFunctionCases(vm, failure, *methodCore, methodSource, *method);
    }
};

namespace JITCache {

// UCBTwinGeneration.h declares it for testjitcache.
bool runCoreCodecSelfTest(VM& vm, String& failure)
{
    return CoreCodecSelfTestAccess::run(vm, failure);
}

} // namespace JITCache

} // namespace JSC

#endif // ENABLE(JITCACHE_TWINS)
