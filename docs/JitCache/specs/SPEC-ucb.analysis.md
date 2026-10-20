# SPEC-ucb.analysis: a module's analysis

Part of [SPEC-ucb.md](SPEC-ucb.md), which indexes it; the main file's facts and invariants apply here. This file specifies how a module's analysis travels with its body: what the analysis is (section 1), the native facts the design rests on (section 2), the `ucb.analysis` section, which holds it in Bun's self-contained module-info form (section 3), its capture (section 4), its import through the path Bun's ESM bytecode builds use (section 5), strict checks and failures (section 6), the edits (section 7), the twin (section 8), options and invariants (section 9), tests (section 10), bench obligations (section 11) and tasks (section 12). [SPEC-ucb-history.md](SPEC-ucb-history.md) records why the decisions that are not obvious were taken; it binds nothing.

## 1. The analysis

Before a module links, the loader builds its record from an analysis of its text: the requested modules with their import attributes and phases, the import and export entries, the star exports, and its `import.meta` and top-level-await flags. Bun's record also has a TypeScript flag, which the analysis parse leaves clear and the section carries as it is. JSC builds the record with an analysis parse of the whole text (FA1). A module whose key matches gets the analysis from its body instead, and Bun builds the record from it as it does for an ESM bytecode build (section 5). The record keeps no declared names (FA2), so none travel. Instantiation, live imports, TDZ and top-level await run on the record as Bun runs them, and the provider keeps the module's text.

## 2. Native facts

- FA1. `JSModuleLoader::makeModule` (`runtime/JSModuleLoader.cpp`) switches on the provider's `sourceType()`. For `Module` it parses the text with `parseRootNode<ModuleProgramNode>` in `SourceParseMode::ModuleAnalyzeMode`, rejects a parse error with `attachErrorInfo`, runs `ModuleAnalyzer::analyze` (`parser/ModuleAnalyzer.cpp`), reports the source's `memoryCost()` on the record with `Heap::reportExtraMemoryAllocated` (under `USE(BUN_JSC_ADDITIONS)`) and fulfills its promise with the record. `ModuleAnalyzer`'s constructor creates the record with `JSModuleRecord::create` and the root node's `features()`. The walk (`parser/NodesAnalyzeModule.cpp`) appends a requested module per import or export-from declaration, deduplicated per phase by specifier and attribute type (`ModuleAnalyzer::appendRequestedModule`), adds a `Single` or `Namespace` import entry per import specifier, an indirect or namespace export entry per export-from specifier and a star export per `export *`. `analyze` then adds, for each exported variable of `varDeclarations()` and then of `lexicalVariables()` (`ModuleAnalyzer::exportVariable`), a local entry, a namespace entry for an exported namespace import (a local entry when that import is deferred) or an indirect entry for another exported import, and calls `setHasTLA(usesAwait())`. The parser takes `import defer` only before `* as` (`Parser::parseImportDeclaration`), so only namespace imports have phase `Defer`. Under `USE(BUN_JSC_ADDITIONS)`, `ScriptFetchParameters::parseType` maps `"json"` to `JSON`, `"webassembly"` to `WebAssembly` and any other non-empty string to `HostDefined`, so `tryCreateAttributes` yields null attributes when there is no `type`, or `JSON`, `WebAssembly` or a `HostDefined` type with its string, never `Text` or `JavaScript`; an entry without attributes has type `JavaScript`, and an empty type is a `TypeError`. Besides the parser's options, the analysis reads `useImportText`, only for a `Text` type, which this build never produces, and `dumpModuleRecord`, which only prints. The analysis parse also records the text's `sourceURL` and `sourceMappingURL` directives on the provider (`Parser::parse`, for a parse mode that is not a function's); the module's UCB request records them again before any frame of the module runs, through its parse or through `CodeCache::recordParseFromUnlinkedCodeBlock` on a CodeCache hit or an import.
- FA2. The record's containers keep insertion order: `requestedModules()` is a `Vector`, `importEntries()` and `exportEntries()` are `OrderedHashMap`s keyed by local name and export name, and `starExportEntries()` is an `OrderedHashSet` (`runtime/AbstractModuleRecord.h`). Its names are atoms, except `starDefaultPrivateName`, the local name of an anonymous default export, and `starNamespacePrivateName`, a namespace import's import name. Both are builtin private names whose hash comes from their literal (`SymbolImpl::StaticSymbolImpl`, `builtins/BuiltinNames.cpp`), and an atom's hash comes from its content, so the hash order in which `exportVariable` meets the declarations is the same in every process, and the analysis is a function of the text and the parse options. Nothing removes an entry once the analysis has added it. The native path later reads the requested modules to load and link; the import entries to resolve imports and to number each import binding's slot in import-entry order (`JSModuleRecord::importSlotNames`), which linked code embeds; the indirect exports at link, where `m_isTypeScript` turns a missing export into no error (`CyclicModuleRecord`); the export entries and star exports to resolve exports and build the namespace; `hasTLA()` to evaluate asynchronously; and `features() & ImportMetaFeature` in `CyclicModuleRecord::initializeEnvironment`, the only read of a record's features in JSC or Bun. The record keeps no declared names: the module environment comes from the executable's symbol table (`ModuleProgramExecutable::moduleEnvironmentSymbolTable`), which the UCB gives, and the analysis reads declarations only to make local exports.
- FA3. `Zig::SourceProvider::create` (`src/jsc/bindings/ZigSourceProvider.cpp`) gives the provider `SourceProviderSourceType::BunTranspiledModule` when its `ResolvedSource` carries `module_info` or `is_prelinked_module`, and the constructor takes the `module_info` pointer. The type is `SourceProvider::m_sourceType`, which the constructor sets and nothing changes later. `makeModule` sends such a provider to `Bun__analyzeTranspiledModule` (`src/jsc/bindings/BunAnalyzeTranspiledModule.cpp`), which builds the record with `zig__ModuleInfoDeserialized__toJSModuleRecord` (`to_js_module_record` in `src/bundler_jsc/analyze_jsc.rs`) through `JSC_JSModuleRecord__create` and the `JSC_JSModuleRecord__add*` functions: features `ImportMetaFeature` or none, `m_isTypeScript` and `setHasTLA` from the flags, then the entries in the order the bytes give them. It frees the module info unless `Bun::IsolatedModuleCache::canUse`, reports no extra memory, and resolves its promise with the record, which fulfills at once because the record's structure has a null prototype (`JSGlobalObject::m_moduleRecordStructure`). In `BUN_DEBUG` builds it also runs `fallbackParse` on the text and rejects the module when `dumpRecordInfo` of the two records differs. A `BunTranspiledModule` provider without module info that is not prelinked goes through `fallbackParse` alone, which skips JSC's `attachErrorInfo` and memory report.
- FA4. Module info comes from Bun's printer (`analyze_transpiled_module` in `src/js_printer/lib.rs`) in two cases. `bun build --compile --bytecode` with ESM output (`LinkerOptions::generates_module_info`, `src/bundler/LinkerContext.rs`) embeds each module's body against a slot table the executable's modules share, read with `ModuleInfoDeserialized::create_with_table` in `src/runtime/jsc_hooks.rs`; the prelinked graph exists only beside it. Under test isolation (`VirtualMachine::use_isolation_source_provider_cache`, `bun test --isolate` or `--parallel`), the runtime transpiler records the self-contained form that `ModuleInfoDeserialized::create` reads, and its cache entry keeps it as `esm_record` (`create_from_cached_record`). The printer sets the TypeScript flag for a TypeScript loader and records an import used only as a type as `ImportInfoSingleTypeScript`, so its records differ from the analysis parse's. Every other ES module's provider is `Module`, and JSC's analysis parse runs.
- FA5. Bun's self-contained form, which `ModuleInfoStringTable::parse` and `Body::parse` read (`src/bundler/analyze_transpiled_module.rs`) and section 3 lays out, carries every form FA1 lists: null, `JSON`, `WebAssembly` and host-defined attributes with the host string, both phases, `Single` imports, `Namespace` imports in either phase, local, indirect and namespace exports, star exports, the two private names as sentinel ids that `getFromIdentifierArray` maps back, any string, 16-bit ones as UTF-16, and the three flags. It drops the root's other `CodeFeatures` bits, which nothing reads (FA2). Bun's builder makes each name an atom with `AtomStringImpl::add` (`JSC__IdentifierArray__setFromChars`), which returns the atom the parse holds.
- FA6. Bun creates a module's provider in the loader's fetch step (`src/jsc/bindings/ModuleLoader.cpp`), on the VM's thread, before `makeModule` runs and before the module's UCB request, which comes at link: `JSModuleRecord::getOrMakeExecutable`, `ModuleProgramExecutable::tryCreate`, `getUnlinkedCodeBlock`, `CodeCache::getUnlinkedModuleProgramCodeBlock`. Every input of the module's key (SPEC-ucb.md section 3.7) is known in `Zig::SourceProvider::create`: the kind and the specialization are fixed, `ModuleProgramExecutable`'s constructor sets the strict bit and no with-scope bit, `getUnlinkedCodeBlock` passes `globalObject->defaultCodeGenerationMode()` for an executable without a symbol table, and the root source digest is the digest of the text, which the provider adopts unchanged from `ResolvedSource::source_code` and which the record's `SourceCode` spans. JITCache can therefore know the key when Bun chooses the source type.
- FA7. When the module's UCB request runs inside `ModuleProgramExecutable::tryCreate`, the executable's `linker()` is the record whose `getOrMakeExecutable` created it (`m_linker`, a `Weak` the constructor sets); `NodeVMSourceTextModule`, behind `vm.SourceTextModule`, creates its executable with none. The provider's type at that request tells which path built the linker: `Module`, JSC's analysis parse in `makeModule`; `BunTranspiledModule`, Bun's module info (FA3). Making a `Weak` counts as a heap allocation (`WeakSet::allocate` calls `Heap::didAllocate`, which can run `performIncrement`), while destroying one only clears it (`WeakSet::deallocate`). A record the parse built is never prelinked, so its entry accessors materialize nothing (`AbstractModuleRecord::ensurePrelinkedEntriesMaterialized`).
- FA8. Every other reader of the source type treats `BunTranspiledModule` as `Module`: the assertion in `ModuleProgramExecutable`'s constructor, `getSourceType` in `runtime/Completion.cpp`, `CachedSourceProvider`, the source-type mapping in `runtime/AbstractModuleRecord.cpp`, `SourceProvider::isModuleType`, the inspector's script type and `DebuggerParseData`. In Bun, `StandaloneGlobalObject::moduleLoaderFetch` takes its own path only for a module that `Bun__standaloneModuleHasModuleInfo` names, which only a `--bytecode` executable has, and `jsFunctionIsolatedModuleCacheSourceType` reports the type to Bun's tests.

## 3. The section `ucb.analysis`

`ucb.analysis` holds one module's analysis as Bun's self-contained module info: the bytes `ModuleInfoDeserialized::create` reads, a string table followed by a body, with nothing before or after (FA5), so Bun takes the section as it is ([history](SPEC-ucb-history.md#a-modules-analysis-goes-through-buns-module-info)). Only a body whose key has identity kind `Module` carries one, and only when its capture had an analysis (section 4.2). Integers are little-endian. A width is the smallest of 1, 2 and 4 bytes that holds a value: W is the width of the blob's length and V the width of n + 1, as Bun's printer computes them (`int_width` in `src/js_printer/lib.rs`).

The string table, with n strings:

| offset | size | field |
|---|---|---|
| 0 | 1 | W, the width of the blob's length |
| 1 | 3 | 0 |
| 4 | 4 | n |
| 8 | W·(n+1) | each string's offset in the blob, in id order, then the blob's length |
| 8 + W·(n+1) | 0 or 1 | a zero byte when W·(n+1) is odd |
| then | the blob's length | the blob |

The blob holds the strings in id order. A string whose units are all at most `0xFF` is the byte 1 followed by its units as Latin-1 bytes; any other is the byte 0, a zero byte when the next blob offset is odd, and its units as UTF-16LE. The blob starts at an even offset of the section and every UTF-16 string at an even blob offset. A module without strings still writes the table: `01 00 00 00`, `00 00 00 00`, the offset `00` and one zero byte.

The body, with q requested modules and r records:

| offset | size | field |
|---|---|---|
| 0 | 1 | flags: bit 0 `import.meta`, bit 1 TypeScript, bit 2 top-level await |
| 1 | 1 | V, the width of n + 1 |
| 2 | 2 | 0 |
| 4 | 4 | q |
| 8 | 4 | r |
| 12 | r | per record: its kind, its fetch kind shifted left by 3, its same-name bit shifted left by 6 |
| 12 + r | q | per requested module: its phase (1 for `Defer`), its fetch kind shifted left by 1 |
| 12 + r + q | to the end | ids of V bytes: per requested module its specifier, then its host type when its fetch kind is 4; then per record the ids its kind lists, then its host id when its fetch kind is 4 |

An id below n names a string; n names `starNamespacePrivateName` and n + 1 `starDefaultPrivateName`. Fetch kind 0 means no attributes for a requested module and type `JavaScript` for an entry; 1 is `JavaScript`, 2 `WebAssembly` and 3 `JSON`; 4 is host-defined and is followed by a string id, which for a requested module is its type string and which Bun ignores for an entry, since an entry keeps only the type `HostDefined`.

| kind | what Bun's builder adds | ids |
|---|---|---|
| 0 | an import entry `Single`, phase `Evaluation` | module name, import name, local name; the local name is left out when the same-name bit says it is the import name |
| 1 | an import entry `SingleTypeScript` | as kind 0 |
| 2 | an import entry `Namespace`, phase `Evaluation`, import name `starNamespacePrivateName` | module name, local name |
| 7 | as kind 2, phase `Defer` | module name, local name |
| 3 | an export entry `Indirect`, or `Namespace` when the import name is `starNamespacePrivateName` | export name, import name, module name |
| 4 | an export entry `Local` | export name, local name |
| 5 | an export entry `Namespace` | export name, module name |
| 6 | a star export | module name |

`encodeModuleAnalysis` writes one byte string per analysis, so equal analyses give equal bytes in any process (IA3):

- the requested modules in the record's order, then as records the import entries, the export entries and the star exports, each in the record's order;
- one id per distinct string, so strings with equal code units share an id, whatever their storage and whether they are atoms (a host-defined type is a plain `String`, and it can equal a name); ids are numbered in order of first use, over the ids in the order the body writes them;
- the same-name bit exactly when a kind 0 record's local name is its import name;
- for a requested module, fetch kind 0 for null attributes, 2 for `WebAssembly`, 3 for `JSON` and 4 with its `hostDefinedImportType()` for `HostDefined`; for an entry, 0 for `JavaScript`, 2 and 3 as above, and 4 with the entry's module name for `HostDefined`; 0 for a local export;
- import kind 0 for `Single` in phase `Evaluation`, and 2 and 7 for `Namespace` in phase `Evaluation` and `Defer`; export kinds 4 for `Local`, 3 for `Indirect` and 5 for `Namespace`;
- flag bit 0 from `features() & ImportMetaFeature` and bit 2 from `hasTLA()`.

The encoder writes only bytes that SA1 accepts (section 6), so it fails with `Unrepresentable` on a record that SA1 would reject once written or that this form cannot carry exactly: a requested module or entry of type `None` or `Text`; a requested module whose attributes have type `JavaScript`; a `SingleTypeScript` import; a `Single` import in phase `Defer`; a `Namespace` import whose import name is not `starNamespacePrivateName`; an indirect export whose import name is `starNamespacePrivateName`; `m_isTypeScript` set; a null identifier in a slot it writes; and a symbol in a slot it writes, except `starDefaultPrivateName` as a local export's local name. The analysis parse produces none of them (FA1).

## 4. Capture

### 4.1 Where the analysis comes from

The producer serializes the analysis when it captures the module's body. Until then the registry keeps only a weak handle on the record the analysis parse built, the module's analysis source, in the UCB's record (`UCBRecord::analysisSource`, SPEC-ucb.md section 6.1), and the handle goes with that record ([history](SPEC-ucb-history.md#the-producer-captures-the-analysis-the-parse-built)). Like the registry's other fields that only production reads, it is not charged to the producer limit (THREAD Session). A record Bun built from module info, its printer's or JITCache's, is never a source, since its provider's type is `BunTranspiledModule` (IA2).

The engine's `keepModuleAnalysisSource` (SPEC-ucb.md section 7.7) sets the source. A `Module` request calls it at three points: right after `recordGenerated` records the UCB (SPEC-ucb.md section 7.3.4), right after `importBody` records it (SPEC-ucb.md section 7.3.1, step 12), and at the start of `attachLive`, before its first check (SPEC-ucb.md section 7.3.3). The last call comes before `attachLive`'s checks because the first of them turns away every VM that does not import, a Producer included, and a Producer must still replace a dead source there. The step acts only in a VM whose production is active, when the request's provider has type `Module` and its executable's `linker()` is set (FA7):

1. When `needsModuleAnalysisSource(ucb)` is false, because the UCB has no record or its record holds a live source, the step ends.
2. Make a `Weak<JSModuleRecord>` of the linker, with no lock held. Making one counts as a heap allocation, which can run incremental marking (FA7), so it never happens under the registry's lock (SPEC-ucb.md section 6.3).
3. `setModuleAnalysisSource(ucb, WTF::move(weak))`, which stores the handle in the record and destroys a dead one it replaces after unlocking.

A later request that the CodeCache serves the same UCB thus replaces a source that died and keeps a live one. The registry declares both calls, and `moduleAnalysisSource`, which a capture reads (SPEC-ucb.md section 6.1).

### 4.2 `buildSections`

For a module core, `buildSections` (SPEC-ucb.md section 8.2) adds a step after the identity section's:

1. With a live source (`moduleAnalysisSource`), `encodeModuleAnalysis` of it, counting `analysisCaptured`. `Unrepresentable` leaves the body without the section and counts `analysisUnrepresentable`; `BudgetRefused` returns `BudgetExceeded`. With strict on, SA3 checks the bytes; its scratch is not charged, as SA1's is not at step 2.
2. Otherwise, the saved body at the record's key, through `VMState::readBodySection(key, SectionKind::UCBAnalysis)` (R-INT-A2). A non-empty `ucb.analysis` is charged and copied, counting `analysisCopied`. This keeps the analysis of a module whose record a ConsumerProducer built from that section, and of one whose source died. With strict on, SA1 checks the copy, and a failure returns `SavedAnalysisInvalid` with SA1's `failure` text as the error's `detail`. SA1's scratch is not charged: it runs only with strict on, it is linear in the section the step has just charged, and `validateModuleAnalysis` frees it before it returns. `Absent` and an empty section leave the body without one. `Unusable` means the integrator found the saved body damaged and has raised invalid material, which ended production, so the call returns `SavedAnalysisUnusable`: the glue raises nothing more and commits nothing ([history](SPEC-ucb-history.md#a-damaged-saved-analysis-ends-the-capture)).
3. `UCBSections::analysis` holds the bytes, empty when there are none, and the glue commits a non-empty one with the other sections (R-INT-A5).

Each failure releases what the call charged.

A capture without an image (SPEC-ucb.md section 8.3) takes the same step, so under the delta capture `all` a module whose top level ran once without compiling carries its analysis. The encoder reads the record's containers and atoms on the VM thread with JS paused, allocates no cell and stops for no collector (I15); the record does not change after its analysis (FA2), and its accessors materialize nothing (FA7). It computes the section's size first and charges it once, charges its table of names before each allocation, and releases all of it when it fails.

`UCBCapture.h` (SPEC-ucb.md section 8.1) gives `UCBSections` its `Vector<uint8_t> analysis`, charged with the rest of the capture; `UCBCaptureFailure` its `SavedAnalysisInvalid`, which the glue raises as invalid material at `ucb.analysis` with the error's `detail` (SPEC-integrator.md section 4.5), and `SavedAnalysisUnusable`, which it does not raise; and `UCBCaptureError` its `detail`, which only `SavedAnalysisInvalid` sets.

### 4.3 Interface

```cpp
// ucb/UCBAnalysis.h
enum class AnalysisEncodeFailure : uint8_t { Unrepresentable, BudgetRefused };

// A module's analysis in Bun's self-contained module-info form, from a record that JSC's analysis parse built. The bytes
// stay charged to the budget; a failure leaves nothing charged.
std::expected<Vector<uint8_t>, AnalysisEncodeFailure> encodeModuleAnalysis(VM&, const JSModuleRecord&, ProducerBudget&);
```

## 5. Import

### 5.1 The lane's entry

Bun calls `importModuleAnalysis`, which the integrator declares with `ModuleAnalysisImport` in `JITCacheBun.h`, the exported header of the entry points only Bun's integration calls (R-INT-A3). This lane defines the function in `ucb/UCBAnalysis.cpp`, and `ucb/UCBAnalysis.h` includes `JITCacheBun.h` and declares neither again. The function takes the global object, the provider's whole text as `source` and a `suppliedDigest`, null when the provider supplies none; it runs on the VM thread with the API lock held. Its result holds `sourceDigest`, the digest the module's key was built from, and `moduleInfo`, the body's `ucb.analysis` bytes, empty when there are none. `std::array<uint8_t, 32>` is `Digest256` (SPEC-ucb.md section 3.1), so the lane's code passes its digests through unchanged.

1. Without JITCache state, or with imports disabled (R-INT-1), return empty.
2. The source digest. With strict off it is `*suppliedDigest` when given and `sourceDigest(source)` otherwise. With strict on it is `sourceDigest(source)`, and a supplied digest that differs is invalid material at `ucb.supplied-digest` (SA2, S2's check made before the provider exists), which returns empty. The step counts the hashes it makes: `sourceDigests` when no digest was supplied, and `suppliedDigestVerifications` when the hash checks a supplied one (SA2). A supplied digest taken without a hash counts nothing here.
3. The key: `BodyKey::make(IdentityKind::Module, CodeForCall, globalObject.defaultCodeGenerationMode(), rootIdentityDigest(IdentityKind::Module, digest, StrictModeLexicallyScopedFeature, std::nullopt))`, the key the module's request computes (FA6).
4. With `jitCacheImportModuleAnalysis` off (section 9), return the digest alone.
5. `VMState::readBodySection(key, SectionKind::UCBAnalysis)` (R-INT-A2). `Absent` counts `analysisMisses` and a `Found` with an empty span `analysisAbsent`; both return the digest alone. `Unusable` returns empty, the integrator having raised the fault.
6. With strict on, SA1; a failure is invalid material at `ucb.analysis` and returns empty.
7. Copy the section into `moduleInfo`, count `analysisImports` and return.

With a bench report open, the function records the `module-analysis` event (R-INT-A6) from step 3 to its return: outcome `off` at step 4, `miss` for `Absent` and `absent` for an empty section at step 5, `invalid` for `Unusable` at step 5 and for SA1's failure at step 6, and `hit` at step 7. The returns before step 3 record no event.

The module's request takes the digest back from the provider (section 5.2) and counts it in `suppliedSourceDigests`, as it counts any digest it takes from a provider (SPEC-ucb.md section 6.5). So in a VM that imports, each such module counts once in `suppliedSourceDigests`, at its request, and its text counts once in `sourceDigests`, at step 2, when nothing supplied a digest ([history](SPEC-ucb-history.md#module-digests-count-where-the-work-happens)).

The import compares no context: the analysis is a function of the text and the parse options (FA2), which the key's source digest and the header's must-match options pin, and every module whose source spans its provider has the same context (SPEC-ucb.md section 3.7). It records nothing in the registry, attaches no pending import and settles no request; the module's request runs later as SPEC-ucb.md section 7 says.

### 5.2 Bun's provider

Integrator task 14 moves the text's `transferToWTFString()` and `sourceURLString` to the top of `Zig::SourceProvider::create`, above the choice of source type, and calls `didLoadAppBytecode` right after them (SPEC-integrator.md section 11.2). Task A4 adds the lines below right after that call, before the choice of source type. They run for a provider that `create` would make as `Module` and that carries no bytecode: the `sourceType` argument is `Module`, the `ResolvedSource` has no `module_info`, is not `is_prelinked_module` and not `isCommonJSModule`, its `bytecode_cache` is null and its text is not null. `supplied` points at the `ResolvedSource`'s digest, as a `std::array<uint8_t, 32>`, when its `has_jitcache_source_digest` is set (SPEC-ucb.md section 7.2.6), and is null otherwise:

```cpp
// JITCache: a module whose saved body carries its analysis gets it here, so the loader builds its record without
// parsing the text.
auto jitCacheAnalysis = JSC::JITCache::importModuleAnalysis(*globalObject, string, supplied);
if (jitCacheAnalysis && !jitCacheAnalysis->moduleInfo.isEmpty()) {
    resolvedSource.module_info = zig__ModuleInfoDeserialized__createFromBytes(jitCacheAnalysis->moduleInfo.span().data(), jitCacheAnalysis->moduleInfo.size());
    ASSERT(resolvedSource.module_info);
}
```

A provider that carries bytecode, from an app built with `--bytecode`, with or without `--compile`, or from Node's compile cache, asks for no analysis: the native path decodes its module's UCB, which then gets no key (SPEC-ucb.md section 7.2.5), and nothing in decoded code imports, so the module's analysis is parsed natively too ([history](SPEC-ucb-history.md#a-module-that-carries-bytecode-imports-no-analysis)).

The rest of `create` runs as before, so a provider holding that module info gets `BunTranspiledModule`, and otherwise the provider and JSC's analysis parse stay as they are (IA1). After construction, `create` sets the provider's `m_moduleInfoFromJITCache` when it took JITCache's module info and, whenever `importModuleAnalysis` returned, sets the digest the provider keeps for `jitCacheSourceDigest()` (SPEC-ucb.md section 7.2.6) to `sourceDigest`. The module's request then takes its root source digest from the provider and reads no text (form 2 of SPEC-ucb.md section 3.5). With strict on, S2 verifies that digest at the module's import as it verifies any supplied digest, so strict reads each module's text twice.

`zig__ModuleInfoDeserialized__createFromBytes`, a new `extern "C"` function beside `zig__ModuleInfoDeserialized__deinit` in `src/bundler/analyze_transpiled_module.rs`, declared beside it in `BunAnalyzeTranspiledModule.h`, returns `ModuleInfoDeserialized::create`'s box as the raw pointer that `zig__ModuleInfoDeserialized__deinit` frees, or null when `create` rejects the bytes. `create` copies the bytes, so the vector can go once the call returns. Bytes the encoder wrote are never rejected; debug builds assert it, and a release build loads the module natively.

### 5.3 Bun's record

`Bun__analyzeTranspiledModule` builds the record from JITCache's module info as from any other (FA3). For a provider whose `m_moduleInfoFromJITCache` is set, it also reports the source's `memoryCost()` on the record with `vm.heap.reportExtraMemoryAllocated` right after building it, as `makeModule` does for a parsed record (FA1). Its other differences from JSC's path change nothing a program observes: the analysis of a text the producer analyzed cannot fail, so neither path reaches `attachErrorInfo`; both promises fulfill at once (FA3); and the module's request records the directives the skipped parse would have recorded before any frame of the module runs (FA1). Under test isolation a provider shared by several global objects keeps its module info (FA3), and each record built from it reports its own memory.

## 6. Strict checks and failures

Normal mode trusts a section the integrity checks passed: `readBodySection` checks the body's framing and the section's checksum (R-INT-A2), and Bun's builder rejects malformed framing by itself. With strict on:

| check | where | what | failure |
|---|---|---|---|
| SA1 | section 5.1 step 6, and the copy of section 4.2 | `validateModuleAnalysis`: the bytes are one whole section of section 3 that an analysis parse can produce, under the rules below | invalid material at `ucb.analysis` |
| SA2 | section 5.1 step 2 | a supplied digest equals the digest of the text | invalid material at `ucb.supplied-digest` |
| SA3 | section 4.2 step 1 | the encoder's bytes pass SA1's rules | `StrictCheckFailed`, a recording fault |

```cpp
// ucb/UCBAnalysis.h
// Whether the bytes are one whole analysis section that the analysis parse can produce; when they are not, failure says
// in words the first property they lack.
bool validateModuleAnalysis(std::span<const uint8_t>, String& failure);
```

SA1's rules:

- The string table: W is the width of the blob's length; the three bytes after it and the padding byte are zero; the first offset is 0, the offsets do not decrease and the last equals the blob's length, which ends inside the bytes; each string starts with tag 1, whose units follow, or tag 0, whose padding byte, when it has one, is zero and whose UTF-16 part has an even length and a unit above `0xFF`; no two strings are equal.
- The body's header: the flags set no bit but bits 0 and 2; V is the width of n + 1; bytes 2 and 3 are zero; r + q tag bytes fit.
- Each record: its kind is 0, 2, 3, 4, 5, 6 or 7; bit 7 is zero; the same-name bit is set only on kind 0; the fetch kind is 0, 2, 3 or 4, and 0 for kind 4. Each requested module: its fetch kind is 0, 2, 3 or 4 and bits 4 to 7 are zero.
- Each id names a string, that is, lies below n, except a local export's local name, which may also be n + 1 (the encoder never writes n, a namespace import's private name, so n fails in every slot); each host id names a string; the ids end exactly at the section's end.
- No two requested modules share a specifier, a type and a phase, fetch kind 0 counting as `JavaScript`; no two import entries share a local name, no two export entries an export name and no two star exports a module name and a type.
- Each import entry names a requested module with its type and phase, and each indirect export, namespace export and star export names one with its type in phase `Evaluation`.

A failure at the import turns cache activity off (THREAD Failures), and Bun creates the provider as without JITCache. A copy that fails is invalid material and ends the capture; an encoder that fails SA3 ends production (THREAD Session). A saved section that the integrator finds damaged ends the capture as `SavedAnalysisUnusable`, the integrator having raised that invalid material itself (section 4.2).

## 7. Edits

No native file of JSC changes but `runtime/OptionsList.h`, which gains the switch of section 9. The lane adds `ucb/UCBAnalysis.h` and `ucb/UCBAnalysis.cpp`, and its registry, engine and capture files gain the members SPEC-ucb.md sections 6.1, 7.7 and 8.1 declare for the analysis. The integrator provides:

- R-INT-A1. A section kind `ucb.analysis` of this lane that `isSectionRequired` never requires; under `Full`, the container's B7 accepts it only in a body whose key has identity kind `Module`.
- R-INT-A2. `VMState::readBodySection(key, SectionKind::UCBAnalysis)`, for section 5.1 step 5 and section 4.2 step 2, backed by the container's read of one section (SPEC-integrator.container.md section 7.6): it follows the root, runs `open`'s step 1 and the record's header checks, then B1 to B7 and B8 on that one section in the session's mode, and returns a `SectionLookup`: `Absent`, `Unusable` after raising the fault as `openBody` does, or `Found` with the section's span, empty when the body lacks it, borrowed from the mapping that the result keeps alive. It counts no `bodyOpens`, runs on the VM thread, allocates no cell and is called without the registry's lock. It checksums no other section, because the module's request opens the whole body moments later and a large bundle's core is megabytes ([history](SPEC-ucb-history.md#the-analysis-import-reads-one-section)).
- R-INT-A3. `JITCacheBun.h`, an exported header of the entry points only Bun's integration calls, declares `struct ModuleAnalysisImport { std::array<uint8_t, 32> sourceDigest; Vector<uint8_t> moduleInfo; }` and `JS_EXPORT_PRIVATE std::optional<ModuleAnalysisImport> importModuleAnalysis(JSGlobalObject&, StringView source, const std::array<uint8_t, 32>* suppliedDigest)` with the meaning section 5.1 gives them, with WTF and standard types only (`JSGlobalObject` forward-declared), and says in its comment that the bytes are Bun's module-info format. It exports the function, which this lane defines. `JITCacheAPI.h` declares neither, and the integrator's include rules let `ucb/UCBAnalysis.h` include `JITCacheBun.h`.
- R-INT-A4. `JITCacheTwinsHost.h` exports, under `ENABLE(JITCACHE_TWINS)`, `void verifyModuleAnalysisTwin(VM&, JSModuleRecord& fromArtifact, JSModuleRecord& fromParse)`, which reports `firstModuleAnalysisDifference` to the VM's open twin report as a UCB difference named `analysis` and does nothing without a report.
- R-INT-A5. The capture glue commits `UCBSections::analysis` when it is not empty, raises `SavedAnalysisInvalid` as invalid material at `ucb.analysis`, and on `SavedAnalysisUnusable` raises nothing, the fault being raised already, and commits nothing; R-INT-2's steps gain `ucb.analysis`.
- R-INT-A6. A bench event `module-analysis` (SPEC-integrator.harness.md section 9.2) with the key, the outcome (`hit`, `miss`, `absent`, `off` or `invalid`), the section's bytes and the thread CPU time from step 3 of section 5.1 to its return, which this lane records (section 5.1).
- R-INT-A7. The jsc shell's `jitcacheReadSection` and `bun:jsc`'s twins export `jitcacheRewriteSection` (SPEC-integrator.harness.md section 5) reach `ucb.analysis`.

Bun (`~/bun`) edits of this lane:

| file | members | section |
|---|---|---|
| `src/jsc/bindings/ZigSourceProvider.cpp` | `Zig::SourceProvider::create` | 5.2 |
| `src/jsc/bindings/ZigSourceProvider.h` | `Zig::SourceProvider`: new `bool m_moduleInfoFromJITCache { false }` beside `m_moduleInfo` | 5.2 |
| `src/bundler/analyze_transpiled_module.rs` | new `zig__ModuleInfoDeserialized__createFromBytes` | 5.2 |
| `src/jsc/bindings/BunAnalyzeTranspiledModule.h` | its declaration | 5.2 |
| `src/jsc/bindings/BunAnalyzeTranspiledModule.cpp` | `Bun__analyzeTranspiledModule`, `fallbackParse` | 5.3, 8 |

`ResolvedSource` keeps its layout: the analysis rides its existing `module_info` pointer.

## 8. The twin

In `ENABLE(JITCACHE_TWINS)` builds, `Bun__analyzeTranspiledModule` sends a record built from JITCache's module info through `fallbackParse`, as `BUN_DEBUG` builds send every record, and `fallbackParse` calls `JSC::JITCache::verifyModuleAnalysisTwin(vm, *resultValue, *moduleRecord)` before its own comparison, so the twin reports before Bun can reject the module. The twin is the record JSC's analysis parse builds in the consumer process from the same `SourceCode`. `firstModuleAnalysisDifference`, declared in `ucb/UCBAnalysis.h` for twins builds, compares in this order:

1. `requestedModules()`: the count, then each one's specifier, phase, whether its attributes are null, their `type()` and, for `HostDefined`, their `hostDefinedImportType()`;
2. `importEntries()`: the count, then each key and the entry's `type`, `phase`, `moduleRequestType`, `moduleRequest`, `importName` and `localName`;
3. `exportEntries()`: the count, then each key and the entry's `type`, `moduleRequestType`, `exportName`, `moduleName`, `importName` and `localName`;
4. `starExportEntries()`: the count, then each module name and type;
5. `features() & ImportMetaFeature`, `hasTLA()` and `m_isTypeScript`.

```cpp
#if ENABLE(JITCACHE_TWINS)
// The first field in which a record built from saved bytes differs from the one the analysis parse builds, comparing
// the requested modules, the import entries, the export entries, the star exports and then the flags, each container
// in its order; null when none does. Feature bits other than import.meta are not compared, since nothing reads them.
String firstModuleAnalysisDifference(const JSModuleRecord& fromArtifact, const JSModuleRecord& fromParse);
#endif
```

Each container is compared in order, since import slots follow import-entry order (FA2). The twin runs wherever a Bun-hosted twins run imports an analysis, whatever strict says.

## 9. Options and invariants

`OptionsList.h` gains a third measurement switch of SPEC-ucb.md section 11, `jitCacheImportModuleAnalysis`, `Bool`, default true, which the integrator adds with the lane's other two (SPEC-ucb.md manifest entry M7): off, `importModuleAnalysis` returns the digest alone, so every module is parsed as without JITCache while its UCB still imports. Captures write the section either way. Besides the switch's free row, the analysis adds no row to options.md: it reads the parser's options, which already shape the UCB, among them the must-match `useImportDefer`, and `useImportText`, which acts only on a type this build never produces (FA1).

- IA1. A provider becomes `BunTranspiledModule` through JITCache only with the analysis of a body at the key its module's request computes. When `importModuleAnalysis` returns no analysis, Bun creates the provider as without JITCache, apart from the digest it keeps.
- IA2. A capture writes `ucb.analysis` only from a record that JSC's analysis parse built (section 4.1) or as a copy of the saved body's section at the same key.
- IA3. The encoder's bytes are a function of the analysis as FA2 lists it, so equal analyses encode equally in any process.
- IA4. A record Bun builds from JITCache's analysis equals, in every field section 8 compares, the record the analysis parse builds from the same text, and reports the same extra memory.

## 10. Tests

### 10.1 C++ tests

`tests/ucb/UCBAnalysisTests.cpp`, `testjitcache` tests built under `ENABLE(JITCACHE_TWINS)`, each in a fresh VM. The fixtures parse a text with `parseRootNode<ModuleProgramNode>` in `ModuleAnalyzeMode` and run `ModuleAnalyzer`, as `makeModule` does. Whether Bun's builder reads the bytes back into the same record is the Bun-hosted twin's to check (section 10.2).

- UA1. Layout. `import { a } from "m"; export const b = a;` encodes to 38 bytes, the table `01 00 00 00 03 00 00 00 00 02 04 06 01 6D 01 61 01 62` and the body `00 01 00 00 01 00 00 00 02 00 00 00 40 04 00 00 00 01 02 02`; a module without imports or exports encodes to the 10-byte empty table and a 12-byte body, whose flags byte is 4 when the module awaits at its top level.
- UA2. Forms: one fixture per form of FA1, together covering a default import, named imports with and without an alias, a namespace import, a deferred namespace import with `useImportDefer` on, a bare import, `json`, `webassembly` and two host-defined types on one specifier, `export default` of an expression, of a named function and of an anonymous class, a local exported under several names, string export names, `export { x } from`, the same `export *` twice, `export * as ns from`, re-exports of an imported binding, of a namespace import and of a deferred one, `import.meta`, top-level `await`, a name with a unit above `0xFF` and a Latin-1 name with one above `0x7F`, a host-defined type equal to a name, which shares the name's id, V at 254, 255, 65,534 and 65,535 strings, and W at blobs of 255, 256, 65,535 and 65,536 bytes: each encoding passes `validateModuleAnalysis`.
- UA3. Determinism: one text parsed in two VMs encodes to equal bytes, and so does the same text held as a 16-bit string by a provider.
- UA4. Refusals: records built through the record API with each form section 3 refuses fail with `Unrepresentable`, a refusing budget fails with `BudgetRefused`, and neither leaves a charge.
- UA5. `validateModuleAnalysis` rejects a crafted section for each rule of SA1, and every prefix of UA2's form sections and of its 254/255-string and 255/256-byte width fixtures; the prefixes of the larger width fixtures are not checked, since they add no case the smaller ones lack and checking each would take time quadratic in sections of hundreds of kilobytes.
- UA6. `firstModuleAnalysisDifference` names each field of section 8 changed alone on a copy of a record, and reports nothing for two parses of one text or for records whose features differ only in another bit.
- UA7. `importModuleAnalysis` with `readBodySection` stubbed, each configuration on its own VM: empty without state, in a Producer and with activity off; the key it reads equals the key the module's request records for the same text once the module links; a key without a body returns the digest alone (`analysisMisses`), as do a body without the section (`analysisAbsent`) and the option off; a body with one returns its bytes (`analysisImports`); with strict on, a malformed section is invalid material at `ucb.analysis` and a supplied digest that differs from the text is invalid material at `ucb.supplied-digest`; step 2 counts `sourceDigests` for a text it hashes for want of a supplied digest, `suppliedDigestVerifications` for one it hashes under strict to check a supplied digest, and nothing for a supplied digest it takes under strict off; with a bench report open, each return from step 4 on records one `module-analysis` event with its outcome, `invalid` for the stub's `Unusable` and for a malformed section, and the earlier returns record none.
- UA8. Registry and capture: in a Producer, a module request on a `Module` provider whose executable has a linker keeps that linker as the UCB's analysis source, whether the request generated the UCB or the CodeCache served it; a request on a `BunTranspiledModule` provider, one without a linker and any request in a Consumer keep none; in a ConsumerProducer, a request that imports the UCB keeps one; a later request served the live UCB replaces a dead source and keeps a live one; the source goes with the UCB's record; `buildSections` of a module UCB writes its live source's encoding, with `BodyImage::Absent` as with `Present`, copies the saved body's section without one, and writes none when neither exists; and with `readBodySection` stubbed to answer `Unusable`, it returns `SavedAnalysisUnusable` and leaves nothing charged; and, with strict on, stubbed to answer a section SA1 rejects, it returns `SavedAnalysisInvalid` with the text `validateModuleAnalysis` gives for those bytes as its `detail`, counts the invalid material and leaves nothing charged.

### 10.2 JS tests

In `JSTests/jitcache/ucb/`, Bun-hosted (`// jitcache-host: bun`) unless marked jsc, with their modules in `JSTests/jitcache/ucb/module-analysis/`. "Statistics" are `jitcacheUCBStatistics()`.

| test | checks |
|---|---|
| `module-analysis.mjs` | a graph covering UA2's forms with `json` and host-defined (`text`, `toml`) attributes in place of `webassembly`, in `Producer; Consumer` and `Producer; ConsumerProducer`: in the importing run every module's analysis imports (`analysisImports` equals the module count), each twin passes, and the output (each namespace's keys and values, `import.meta.url` relative to the scratch directory, the order in which top-level awaits settle, the attribute imports) equals the JITCache-off run; with `BUN_JSC_jitCacheImportModuleAnalysis=0` in the consumer no analysis imports and the output is the same; after the graph, three modules the script writes, with 254 and 255 strings and an export name of 65,536 characters, give string ids of 1 and 2 bytes and a blob length of 4 bytes (65,535 strings is left to UA2) |
| `module-analysis-errors.mjs` | link errors in the consumer, each with the JITCache-off run's message: an import of a missing export, a name two star exports make ambiguous, and a TypeScript module re-exporting a name its source lacks, which the JITCache-off run rejects because the parse's record keeps `m_isTypeScript` false (FA4); the Producer first links a copy of that module's text beside an exporter that has the name, so an importing run builds the module's record from the analysis (`analysisImports`) and the link still fails |
| `module-analysis-miss.mjs` | a module whose text the consumer changes misses (`analysisMisses`) and is parsed; one given a syntax error in the consumer throws what the JITCache-off run throws, which in Bun is usually its transpiler's `BuildMessage`; under the delta capture `compiled`, a module whose top level never compiled has no body and is parsed; an ES module that Node's compile cache serves, in a consumer that calls `module.enableCompileCache()` over a scratch directory the producer filled, carries bytecode, so no analysis counter changes across its load and it is parsed |
| `module-analysis-recapture.mjs` | `Producer; ConsumerProducer; Consumer`: the ConsumerProducer imports the analyses, compiles one module's top level, which loops, and recaptures that body when its baseline compilation finishes, with its analysis copied (`analysisCopied`, read during the script, which only that capture moves), and the Consumer imports it; runs identical |
| `module-analysis-uncompiled.mjs` | modules whose top level runs once without compiling, under the delta capture `all`: their bodies carry the analysis, which the consumer imports (`analysisImports`); `analysisCaptured` moves only in the producer's `delta` at exit, after the script has ended, so the consumer's imports show it |
| `module-analysis-invalid.mjs` | twins only, each case a sequence of its own, `Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer`, over a module that imports nothing, whose top level runs once and which the script imports dynamically, last. The Producer captures the module with its analysis. The Consumer imports the analysis, and the last `module-analysis` event of its report, a `hit`, gives the module's key, which a Bun script has no other way to name; a Producer writes no such event, since its import stops before the key. The ConsumerProducer rewrites `ucb.analysis` at that key through `bun:jsc`'s `jitcacheRewriteSection`, which only a producing VM may call, then imports the module. The cases: an offset width of 3, the TypeScript flag, a kind 1 record, a fetch kind 1, an id past n + 1, n + 1 as an import name, a duplicate export name and a record count one below the records written. Each is invalid material at `ucb.analysis` in run 2 (`jitcache-expect-fault`), the module loads natively and the run equals the JITCache-off run. The module's body has no image, so runs 1 and 2 install nothing and declare `jitcache-expect-no-install` |
| `module-analysis-worker.mjs` | a `Worker` importing the graph's modules: its VM imports their analyses and its twin report passes |
| `module-analysis-shell.mjs` (jsc, twins only) | the shell's producer writes `ucb.analysis` for each captured module body, which `jitcacheReadSection` reads non-empty at the keys of the producer's `generate` events in its bench report (no shell function names a module body's key) and SA3 checks; the shell's consumer imports no analysis (`analysisImports` stays 0) and parses every module, as the shell has no provider that asks |

## 11. Bench obligations

- BA1. Per module, `importModuleAnalysis` from step 3 to its return, plus `zig__ModuleInfoDeserialized__createFromBytes` and Bun's record build, against JSC's analysis parse and `ModuleAnalyzer` on the same text, on the five bench bundles and an unbundled app, on x86_64 and the A1, with warm and cold page caches. The digest is B1's. The decision rests on this gap: JSC's analysis of the five bundles costs 0.5 to 8.3 ms on x86.
- BA2. Startup of the macro bench apps with `jitCacheImportModuleAnalysis` on and off, both importing everything else, beside the same apps built with `--bytecode` and run without JITCache, on the A1.
- BA3. Capture: the encoder's time and the section's bytes per module, the copy's time, and the memory the weak sources hold in a producer.

## 12. Tasks

These join SPEC-ucb.md section 15.3, whose task numbers they use.

- A1, with task 0: `ucb/UCBAnalysis.h` exactly as sections 4.3, 6 and 8 declare it, including `JITCacheBun.h` for section 5.1's entry, after the integrator's task 0, which writes that header (R-INT-A3). The analysis members SPEC-ucb.md sections 6.1, 7.7 and 8.1 declare come with task 0's headers.
- A2: `ucb/UCBAnalysis.cpp`: `encodeModuleAnalysis`, `validateModuleAnalysis`, `importModuleAnalysis` with its `module-analysis` bench event, and, in twins builds, `firstModuleAnalysisDifference`, with UA1 to UA7. Needs tasks 1, 2 and A1 and the switch of SPEC-ucb.md manifest entry M7, which `importModuleAnalysis` reads and SPEC-integrator.md task 1 applies (its M12); `readBodySection` (R-INT-A2) is stubbed until the integrator lands it.
- A3: the producer's parts, each in the task that owns its file: the registry's analysis-source members in task 6 (`UCBRegistry.cpp`), `keepModuleAnalysisSource` and its three calls (section 4.1) in task 7, and the step of section 4.2 in task 9. UA8 lands with the last of them.
- A4: Bun, the table of section 7 and the twin call of section 8, after A2, the integrator's task 14, whose edit of `Zig::SourceProvider::create` comes first (section 5.2), and R-INT-A4; then the JS tests of section 10.2, which also need the integrator's runner.

Manifest M1 gains `jitcache/ucb/UCBAnalysis.cpp`, and `testjitcache_SOURCES` gains `tests/ucb/UCBAnalysisTests.cpp` (SPEC-ucb.md M2). SPEC-ucb.md section 6.5 declares the analysis counters.
