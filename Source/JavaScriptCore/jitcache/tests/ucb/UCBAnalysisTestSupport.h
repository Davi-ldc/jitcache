#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleRecord.h"
#include "ModuleAnalyzer.h"
#include "Parser.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "VM.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/ucb/UCBAnalysis.h"
#include <memory>
#include <optional>
#include <wtf/Ref.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

// What the tests of a module's analysis share: a module's source, the record JSC's analysis parse builds for it, and the
// analysis section the encoder writes from a record or from a text. The names live in a namespace of their own.

namespace JSC::JITCache::Tests::UCBAnalysisTestSupport {

inline SourceCode analysisModuleSource(const String& text, SourceProviderSourceType type = SourceProviderSourceType::Module)
{
    return makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, type);
}

// The record JSC's analysis parse builds for the source, as JSModuleLoader::makeModule builds it; null on a parse or an
// analysis error. The caller holds it.
inline JSModuleRecord* analyzeModule(JSGlobalObject* globalObject, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    ParserError error;
    std::unique_ptr<ModuleProgramNode> node = parseRootNode<ModuleProgramNode>(vm, source, ImplementationVisibility::Public, JSParserBuiltinMode::NotBuiltin,
        StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, SourceParseMode::ModuleAnalyzeMode, error);
    if (error.isValid() || !node)
        return nullptr;
    ModuleAnalyzer analyzer(globalObject, globalObject->moduleLoader(), Identifier::fromString(vm, "jitcache-analysis-module"_s), source, node->features());
    auto record = analyzer.analyze(*node);
    return record ? *record : nullptr;
}

inline std::optional<Vector<uint8_t>> encodedAnalysis(VM& vm, const JSModuleRecord& record)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto bytes = encodeModuleAnalysis(vm, record, budget.get());
    if (!bytes)
        return std::nullopt;
    return WTF::move(*bytes);
}

// The analysis section of a module text, from the parse to the encoder.
inline std::optional<Vector<uint8_t>> encodedText(JSGlobalObject* globalObject, const String& text)
{
    VM& vm = globalObject->vm();
    Strong<JSModuleRecord> record { vm, analyzeModule(globalObject, analysisModuleSource(text)) };
    if (!record)
        return std::nullopt;
    return encodedAnalysis(vm, *record.get());
}

} // namespace JSC::JITCache::Tests::UCBAnalysisTestSupport

#endif // ENABLE(JITCACHE_TWINS)
