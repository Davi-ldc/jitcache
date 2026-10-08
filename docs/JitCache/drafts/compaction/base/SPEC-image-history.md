# SPEC-image history

Rationale and review records for [SPEC-image.md](SPEC-image.md). Nothing here is binding.

## Draft 1 (2026-10-05)

Written from the current THREAD, with the user's settlements of the previous run's gaps applied (strict failures, the `negate` operations and coverage rates in this lane, the module names, VM data that exists lazily or only under an option). No review has run yet.

### Why fixups use the assembler's own writers

THREAD Restoration asks for an image "patched with the assembler's own link writers, which place ARM64 islands". Defining each form by the site the native writer takes (`X86Assembler::linkPointer`, `linkCall`, `linkJump` and their ARM64 counterparts) means the consumer runs exactly the code `LinkBuffer` runs, islands included, and the producer's label translation is `LinkBuffer`'s own `applyOffset`. Three forms cover every reference: a pointer, a near call and a near jump. Far calls are pointers because the native far call is a fixed-width pointer move followed by an indirect call.

### Why explicit helpers instead of tagging `TrustedImmPtr` and `AbsoluteAddress`

Carrying a target inside the immediate types would reach hundreds of `MacroAssembler` operations on two architectures, each of which would have to learn a fixed form. The baseline image embeds references at a few dozen places, almost all through a handful of shared helpers (exception checks, write barriers, structure decodes, slow-path calls, IC slow paths). Annotating those places, with two guards that turn a missed thunk link or pointer argument into an unrecorded compilation, keeps the change small and keeps native emission byte-identical. A missed pointer immediate outside the guarded paths is the remaining risk; the twin comparison catches it because the restored and native images then differ at that site.

### Why C++ functions are code symbols and thunk generators an enum

A `CodeSymbol` is the function's offset from an anchor in the engine object. THREAD's header holds that object's build ID, so the offset is a build-stable symbol, and it needs no table of operations (the Linux build has no `JITOperationList` section). A thunk generator is different: the consumer would call it during import, so its key comes from a closed enum and maps to the generator through `JIT::baselineThunkGenerator`. Slow-path functions are only handed to `ctiSlowPathFunctionStub` as data, so a symbol suffices.

### Why veneers at the end of the allocation

THREAD requires each external conditional branch on ARM64 to go through a local `b`. The inline alternative (`b.!cond` over a `b`) turns every not-taken exception check into a taken branch on the hot path. A veneer per target after the code keeps the conditional branch not taken, shares one `b` among all branches to the same thunk, and leaves compaction to choose the conditional branch's form from internal distances only.

### Why recording never reuses a cached temp

THREAD allows reuse only for the same reference. Reuse windows between labels are short in baseline code, and tagging `CachedTempRegister` with a reference identity would add state to every ARM64 cache path. Invalidating after each reference costs a few instructions at sites that already pay for a three-instruction move. B3 measures it.

### Why side tables are offsets and not fixups

Call and IC return points, switch entries, code-map entries and MathIC locations live in `BaselineJITCode`'s tables, outside the code bytes. Storing them as offsets and rebuilding the tables at import is THREAD's own rule ("a code address travels as an offset into its image or snippet"), and it keeps every fixup a byte range of code.

### String switch ranks

THREAD Capture says internal branches carry no fixup, and Restoration says the consumer "retargets that rank's case jump". The leaf jump of an inline `switch_string` is internal, but which case holds a rank depends on the order of atom addresses in the process, so its displacement depends on the process. The SPEC follows Restoration and the rule's first clause and records it as a provisional choice (section 3.5).

### MathIC regeneration and the allocation fault

THREAD Failures asks the VM thread to raise the fault for a failed MathIC snippet allocation "before its effects are written". In `JITMathIC::generateOutOfLine` the slow call is repointed before the full snippet is allocated, unconditionally, so it is no effect of the failure; the failure's own effect is the native fallback that follows. Calling `didFailExecutableAllocation` at each `didFailToAllocate()` branch raises the fault there, inside the operation, and no capture can run between that point and the fallback.

### The `negate` fix

Add, sub and mul repoint a regenerated IC to a `*ProfiledNoOptimize` operation that takes the IC. Adding `operationArithNegateProfiledNoOptimize` mirrors that and leaves `operationArithNegateProfiled` correct for its callers that pass a profile. DFG and FTL use the non-profiled `operationArithNegateOptimize`, so the change touches baseline code only.

### Baked facts

The code depends on the capability class only through `CannotCompile`, so the comparison tests that predicate. `CodeBlock::capabilityLevel` would memoize into the newborn CB; `computeCapabilityLevel` keeps the comparison free of writes. Taint is compared exactly: a mismatch leaves the CB native, which is the safe outcome even though native sharing tolerates it.

### Capture streams from live memory

THREAD wants a writer with bounded memory and a capture that writes before JS resumes. Streaming the code and snippets from executable memory through a fixed buffer, canonicalizing fixup footprints on the way, avoids copying images into the producer's budget.

### A rebuilt record the budget refuses

In a ConsumerProducer VM the rebuilt record is production memory. If the budget refuses it, the import is still a valid image, so the SPEC lets the install complete and has the glue raise the recording fault afterwards; production ends as THREAD requires, and the consumer side keeps its import.

### Twins

A twin needs a CB that has not run (`JIT::compileAndLinkWithoutFinalizing` asserts it), so the check builds a fresh CB through `ScriptExecutable::newCodeBlockFor` and never installs it. (Superseded in revision 1, which creates the twin through the CB class's `create`.) The producer's compile inputs are snapshotted at compile start, which equals what the emitters read only when no LLInt write races the compilation; producer twins runs therefore turn `useConcurrentJIT` off, a free option.

### Alignment with the lanes written in parallel

The drafts of the CB, ICs and UCB lanes were read only for overlap. The working names `ProducerBudget::tryCharge` and `release`, `didFailExecutableAllocation(VM&)` and `TwinReport` follow theirs, so the integrator sees one requirement per piece. The UCB lane lists class template `ArithProfile` because it adds `restoreBits`; this lane edits only `ArithProfile::emitUnconditionalSet`, a different function, and manifest entry M8 lets the integrator apply it if it treats the class as one unit. The UCB lane's new `numberOfBinaryArithProfiles()` and `numberOfUnaryArithProfiles()` serve this lane's validation too (R-UCB-3).

### Alternatives considered and rejected

- Scanning the linked bytes for addresses: blinding, value folding and compaction make sites unrecoverable, as `compiler.md` notes.
- Classifying immediates by value at emission: a literal can equal an address; THREAD asks emitters to annotate.
- Shipping pre-compaction code and recompacting in the consumer: compaction of thunk branches depends on absolute addresses, and recompaction would change internal offsets after the side tables were recorded.
- Recompiling in the consumer from recorded inputs: that is a native compilation, which the installation bound excludes.

## Revision 1 (review round 1, 2026-10-05)

Round 1 filed four blockers and two majors against draft 1. They reduce to four issues, each confirmed against the code, and none needed THREAD to settle it.

### MathICs without inline code (three reports)

`JIT::emitMathICFast` (both overloads, `JITArithmetic.cpp`) calls the operation directly and adds no slow case when `JITMathIC::generateInline` returns false. That happens on `DontGenerate`, which the `generateInline` of `JITAddGenerator`, `JITSubGenerator`, `JITMulGenerator` and `JITNegGenerator` return when the profile saw only non-numbers, and when `JITAddGenerator::generateFastPath` or `JITMulGenerator::generateFastPath` refuses an operand whose `OperandTypes` cannot be a number. `JIT::privateCompileSlowCases` walks only `m_slowCases`, and `JIT::emitMathICSlow` is the only registrant of the `finalizeInlineCode` link task, so such an IC keeps four null locations and has no slow call. Draft 1 assumed every MathIC had both, so a body with string concatenation would have failed S4 at capture (a recording fault under the default strict) or, without strict, produced a section that V5 rejects.

Every path on which `generateInline` returns true appends at least one slow-path jump (the empty profile's `patchableJump`, an overflow branch or a type branch), so "has inline code" and "has locations" coincide. The revision gives MathIC entries a "no inline code" state: flag bit 2, zero fields, an empty slow-call site, no snippet, no fixup naming the IC. V5, V6, S4 and I7 treat it explicitly, the import creates the IC with its generator and null locations, and T1 covers the sites.

The state is read from the IC at `finishBaselineCompile`, which already runs after the link tasks, instead of a second emitter call: the IC's null locations are the native fact the import must reproduce. A partial set of locations is a programming error, so `OverlappingFixups` became `InconsistentRecord`, covering that case and a missing slow-call fixup.

### Creating the twin CB (one report)

`ScriptExecutable::newCodeBlockFor` `RELEASE_ASSERT`s that the executable has no CB of the requested kind, and the twin check ran after `installCode` had filled that slot. `newReplacementCodeBlockFor` is no substitute: its `CopyParsedBlock` constructor copies `m_metadata` from the installed CB (`CodeBlock::CodeBlock(VM&, Structure*, CopyParsedBlockTag, CodeBlock&)`), so writing recorded metadata into it would overwrite the live CB's. The report's smaller points also held: the twin CB never gets JIT code, so a `MathICRegeneration` keyed on `codeBlock->jitType()` stayed inactive during the replays, and the twin's capability memo is the private `m_capabilityLevelState`.

The revision creates the twin through the CB class's own `create(vm, executable, unlinkedCodeBlock, scope)` under `DeferGCForAWhile`. `generateOutOfLine` now takes its `MathICRegeneration` explicitly behind an unchanged wrapper, and the twin replay passes a twin-mode object bound to the twin's record that seeds each assembler and never faults the VM. Running the hook between native setup and `installCode`, while the slot is still empty, was the other option; it was rejected because the twin's linking, compilation and possible exception would then interleave with the install itself, while direct creation works after it.

The suggested test hook for the capability memo was declined. The twin shares the executable and the source provider with the installed CB, so its own `capabilityLevel()` and `couldBeTainted()` equal the installed CB's, which baked facts already compared with the recorded ones, and the code depends on the level only through `CannotCompile` (N13). The check verifies both instead of writing them.

A discarded twin would also change transported state: `CodeBlock::~CodeBlock` sets `didOptimize` in the UCB's unlinked metadata to `False` while it is indeterminate, unless `vm.heap.isShuttingDown()`, and that value is UCB feedback that travels. `Twins` therefore keeps every twin CB in a `Strong` list that the integrator releases during VM destruction. `VM::~VM` calls `heap.incrementDeferralDepth()` before `Heap::lastChanceToFinalize`, which sets `m_isShuttingDown` before any destructor runs.

Draft 1 also left seed injection unspecified. `BinarySwitch` draws from `m_weakRandom` inside its constructor while it builds the tree, so a twins-build constructor overload takes the recorder and gets its seed from it. `AbstractMacroAssemblerBase::initializeRandom` runs lazily at the first `random()`, so `ImageRecorder::attachTo` can seed the assembler before any draw. The consumer of a twins run also turns `useConcurrentJIT` off, since the check overwrites the live UCB's arithmetic profiles while it compiles.

### PC-relative fixups in the twin comparison (two reports)

The twin is a separate allocation, so every `Call` and `Jump` fixup, including the `ImageOffset` jumps of snippets, holds a different displacement than in the restored image, and ARM64 may route it through other islands (`ARM64Assembler::linkJumpOrCall`). Draft 1 excluded only `MathIC`, `SwitchTableBase` and `SnippetEntry` footprints from the byte comparison, so T2 could never pass. The revision compares canonical bytes, which covers everything outside footprints, fixup lists by logical target, and each side's footprints decoded against that side's own resolution context. Since support, VM, UCB, process and structure-base targets resolve identically in both contexts, the decodes compare their absolute values without a per-kind exception list.

### A refused charge and ARM64 veneers (one report)

On ARM64 only `emitVeneers` linked a deferred conditional jump, and deferring needs storage. Draft 1 said a refused charge stops all recording, which left a choice between an unlinked exception check and uncharged memory. In the revision `deferConditionalJump` links the jump natively, with no veneer, once the recorder has stopped or when the charge for its group is refused. Groups are charged as they grow at deferral, and `emitVeneers` allocates nothing beyond each veneer's fixup record, so it links every jump deferred before the refusal. An `Unrecordable` recorder follows the same rule. Reserving deferral capacity in the first charge was rejected, because the number of conditional external jumps is unknown before emission. T12 now sweeps the limit through a body that throws and regenerates.

## Revision 2 (review round 2, 2026-10-05)

Round 2 filed three blockers and three majors against revision 1. All six held up against the code, and none needed THREAD to settle it.

### The linked size (blocker)

Revision 1 took the image's size from the `BaselineJITCode`'s code reference. `JITCodeWithCodeRef::size` and `MacroAssemblerCodeRef::size` return the handle's `sizeInBytes()`, and the handle can be larger than what `LinkBuffer` wrote. On ARM64, `LinkBuffer::copyCompactAndLinkCode` copies only the compacted `m_size` bytes after `shrink`, and `MetaAllocatorHandle::shrink` rounds up to the allocator's granule. With the libpas JIT heap, `ExecutableMemoryHandle::createImpl` and `ExecutableMemoryHandle::shrink` set the size from `jit_heap_get_size`, a size-class bound, so x86_64 is exposed too even though `LinkBuffer::linkCode` copies the whole padded buffer there. The tail keeps whatever the pool held, so a capture would have carried stale bytes outside any fixup, and T2 would have compared two unrelated tails.

The image and each snippet are now defined by their linked size (`LinkBuffer::size()`), which `finishBaselineCompile` and `didLinkSnippet` record. Capture streams only those bytes and bounds side-table pointers by them; the import allocates, copies and flushes exactly them, as `LinkBuffer::performFinalization` flushes `m_size`. `commit` samples the code's `size()`, the handle's, because `JIT::finalizeOnMainThread` samples `jitCode->size()` and the statistic should keep one measure across native and imported code. N20 records the native facts, and T17 tests bodies whose allocations end past their linked size.

### Footprints checked on every import (blocker)

The writers trust their site. `ARM64Assembler::linkJump` goes through `relinkJumpOrCall<BranchType_JMP>`, which treats a `nop` at the site as the second half of a conditional branch and rewrites the instruction before it, before the allocation for a fixup at site 0. The x86_64 writers store their field without reading the opcode. Revision 1 checked footprint encodings only under strict (its S5), so with strict off a section that passed every V and U check could still make a writer read or write outside the footprint, and I18 was false.

V3 now checks, on every import, that each footprint holds its form's canonical encoding, with exact bytes per form and architecture in section 3.2. The x86_64 `Jump` length comes from the opcode: `E9` at `site-5` or `0F 80` to `0F 8F` at `site-6`, which cannot both match because `E9` is outside `80` to `8F`. A separate x86_64 conditional-jump form was considered and rejected: the opcode already decides the length, so a new form would add a fixup kind without adding information. The old S5 is gone, the read-back after patching is renumbered S5 and stays strict-only, and I18 now also says each writer writes only inside its footprint, apart from ARM64 islands.

### Super-sampler opcodes (blocker)

`op_super_sampler_begin` and `op_super_sampler_end` come only from the `@superSamplerBegin` and `@superSamplerEnd` intrinsics, which lex only where `Lexer` sets `m_parsingBuiltinFunction`: builtin-mode parsing, or `exposePrivateIdentifiers`. That option changes parsing, so THREAD Storage fixes it off, and the UCB lane lists the row. A search of `Source/JavaScriptCore` and Bun's `src/js` finds no builtin that calls either intrinsic; only test hooks that parse builtin-mode text (`$vm.createBuiltin`, the jsc shell's builtin helpers) produce them. The `ProcessAddress` kind, its table, `ImageReference::processAddress`, `add32AtReference`, `sub32AtReference`, their ARM64 immediate rule and the T1 item served nothing a captured body can contain, so they are gone, and the kinds are renumbered to end at 21.

The two emitters now make the record unrecordable. The review suggested reusing `UnannotatedReference`; the SPEC adds `SuperSamplerOpcode` instead, because `UnannotatedReference` names shared-helper paths that baseline emission never takes, while these opcodes do reach it, and a status diagnostic should name the opcode the way `DebuggerOpcode` and `ProfilerOpcode` do. T11 covers it with a C++ test that compiles a builtin-mode function made with `createBuiltinExecutable`.

### The prepared code before commit (major)

The ICs lane's `prepareBaselineICs` takes `const BaselineJITCode&` and checks mold pairing against `m_unlinkedPropertyInlineCaches` before anything is written, while revision 1 exposed the constructed code only through `commit`, which has effects outside the prepared objects. `PreparedImage::code()` now returns that object, valid until `commit` or destruction, and `commit` returns the same object. `moldCount()` was dropped because `code()` subsumes it. R-INT-7 orders `prepareImage` before the other preparations, passes `code()` to the ICs lane, and has a failed later preparation destroy the `PreparedImage` without `commit`, which frees the code and leaves the VM's statistics alone.

### The twins producer side (major)

Revision 1 named the twins data but gave the section no layout, gave the recorder no way to receive CB values, assigned no task to the snapshot, and listed only the unguarded scope kinds among the inputs. A grep of `m_profiledCodeBlock` in the baseline emitters shows what emission reads from mutable state: `m_resolveType` and `m_localScopeDepth` of `resolve_scope`, `m_getPutInfo.resolveType()` of `get_from_scope` and `put_to_scope` at every site including the global kinds, the mode of `get_by_id`, `iterator_open` and `async_iterator_open`, `m_enumeratorMetadata`, the capability level and the taint. The rest is layout the UCB fixes, the profiler opcodes, which leave a body unrecordable, and scope operands, which the code loads at run time. A twin linked fresh in the consumer can resolve a global site differently, so without the recorded types T2 would have failed spuriously.

Section 9.6 now gives `image-twins.baseline` a byte layout and checks W1 to W4. `snapshotCompileInputs` reads every input at compile start and hands it to the twins-only `ImageRecorder::recordCompileInputs`, so the recorder itself still reads no CB. A snapshot needs one call where recording at each read would need edits at ten emitter sites, and it is exact because twins runs turn `useConcurrentJIT` off, which the arithmetic-profile snapshot needs anyway. The capability level and the taint already travel as baked facts. The regeneration log is filled by the native `MathICRegeneration`, whose constructor now receives `callReplacement` and the profile's bits from the `generateOutOfLine` wrapper; each `attach` adds a seed slot.

Producer values are computed at capture by resolving every fixup in the capturing VM, with the same `resolveTarget` the emitters used and the context S1 already builds, so the recorder stores nothing for them, and a ConsumerProducer's recapture carries its own values. The relocation comparison skips artifact targets, whose addresses are each process's own allocations.

Writing recorded values into the twin's metadata raised a problem the review did not mention. The twin stays alive until VM destruction, and `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` reads the structure-or-watchpoint-set union of `get_from_scope` and `put_to_scope` as a `StructureID` for every type except the variable kinds. A type rewritten over a link that stored a watchpoint-set pointer would make the GC decode that pointer. The twin's write therefore clears the union and the operand. A recorded `ProtoLoad` mode is written with zero fields, which is safe because the GC reads a structure ID only from a `Default` entry and reaches `ProtoLoad` entries only through `m_llintGetByIdWatchpointMap`, empty for the twin.

### The ConsumerProducer's twin data (major)

The rebuilt record of a ConsumerProducer import now takes the seeds, the compile inputs and the regeneration log from the imported twins section, so the ConsumerProducer's own regenerations append to the original compilation's log and a fresh consumer's twin replays both processes' regenerations. T15 now has two generations of regenerations. I9 covers `image.baseline` and `baked-facts.baseline` byte for byte, and the twins section except its producer values, which each capture computes in its own VM.

## Revision 3 (review round 3, 2026-10-05)

Round 3 filed one major: the relocation clause of section 17.2, and T3 with it, could not pass on the builds this project makes. The report held up against Bun's build scripts and the engine. One part of it only THREAD can settle, so the revision marks a provisional choice; the rest became duties on the integrator's runner.

### The engine image does not move

Bun's `scripts/build/flags.ts` compiles Bun's own sources with `-fno-pic -fno-pie` (`bunOnlyFlags`) and links Linux executables with `-fno-pic -Wl,-no-pie` (`linkerFlags`), commented "No PIE (we don't need ASLR; simpler codegen)". The local WebKit recipe that `bun build.ts` runs for the `jsc` target (`scripts/build/deps/webkit.ts`) puts `-fno-pic -fno-pie -no-pie` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`, which CMake also passes to the link (the recipe's comment about `try_compile()` probes relies on it), and turns `CMAKE_POSITION_INDEPENDENT_CODE` off. The engine image therefore sits at the same address in every process of a build. `CodeSymbol::address` adds an offset to the anchor's address, so every `Operation` target resolved to the same value in producer and consumer, and the clause would have failed every body that makes a far call.

The report named only far calls, but the engine image also holds static data that targets name. Every `""` constant is `vm.smallStrings.emptyString()`, whose value impl is the static `StringImpl::s_emptyAtomString`, so `x === ""` compiles to a `UCBConstantAtom` fixup whose value lies in the engine's data segment, and so would a `switch` case `""` or a getter named `""`. Grouping targets by kind would put atoms with the UCB, and the exemption the report suggested for far calls alone would have left these pairs failing in a position-dependent build. The revision therefore groups targets by where they resolve, not by their kind.

THREAD Verification computes twins "with every process, VM, UCB, support and Structure-reservation address differing between producer and consumer", and only a position-independent engine lets the engine image move. The revision keeps that requirement and makes twins builds position-independent, marked provisional because it departs from the Bun profile's flags, which the project's build rules leave to Bun. The alternative for the human is to keep those flags and have the clause leave out every target that resolves into an object loaded with a zero load bias. That is safe for such builds, because their production processes never relocate the engine image either, but a reference into the image missing from the record would then go unseen until some build is position-independent, such as one with the separate engine object THREAD's header provides for. Far calls are recorded centrally from `JIT::m_farCalls`, so the exposure there is a C++ function pointer moved some other way; for static atoms it is any emitter that moves an atom outside the census.

### Structure reservation and executable pool

`StructureMemoryManager` reserves the structure heap aligned to its own size, 4 GiB on Linux, and derives `structureIDBase` from its start. The kernel shifts the mapping area by a page-granular random offset of `vm.mmap_rnd_bits` bits: 28 by default on x86_64, up to 1 TiB, which holds about 256 aligned 4 GiB bases, so roughly one producer and consumer pair in 256 would share a structure base. On arm64 kernels left at the 18-bit minimum the offset spans 1 GiB, and two processes nearly always share one. Relaunching until the bases differ would not end there, so the runner now places the reservations apart before `JSC::initialize` makes them.

The pool's page-granular base rarely coincides, but two pools at different bases can overlap, and the two processes generate thunks in different orders (the producer in emission order, the consumer in the order import resolves support keys), so one process's thunk can sit where the other put another. Disjoint ranges rule that out. `jitMemoryReservationAddress` places the pool exactly, and the jsc shell enables restricted options and parses its command line before `JSC::initialize`; a placeholder mapping over the capturing process's ranges works for both reservations. The integrator chooses.

### The heap

The VM's fields, cells, heap atoms and the UCB's profile vectors share one domain. The allocators take their memory from mappings that address randomization places: mimalloc in `release-local`, and the ASan allocator in `debug-local`. As evidence for the latter, the pinned LLVM 21 ASan runtime's x86_64 disassembly holds no `0x500000000000` or `0x600000000000` immediate, which fits a primary allocator space placed at run time; this was not checked against the runtime's source. Allocations made in the same order at startup, such as the VM itself, follow the mapping area's random base, so they coincide only when the two bases do. Cells and buffers made later, in different orders, coincide more rarely still. A coincidence says nothing about JITCache, so the runner repeats a run whose only reports are such pairs; a defect, such as a resolution that ignores its context, recurs.

### T3

T3 described only what the clause detects. It now requires that no run of T2 fails the clause under the runner's layouts, and adds a hook that substitutes the consumer's resolution for one producer value in each domain, which tests the detection itself.

## Revision 4 (system review round 1, 2026-10-06)

The review of the composed design filed three majors against this lane. Two describe one defect, the twin check's blindness to atom-ness, and the third the headers JSC exports to Bun. All three held up against the code, and none needed THREAD to settle it.

### Atom-ness at strict-equality sites (two reports)

`JIT::compileOpStrictEq` and `JIT::compileOpStrictEqJump` choose their inline string fast path through their `tryGetAtomStringConstant` lambdas, which test `isAtom()` on the constant's current `StringImpl`, left operand first. Atom-ness only grows: a natively decoded constant stays plain until a property-key use (`JSString::toIdentifier`, `toAtomString`) or the decoder's string table (`DecoderStringTable::atomFor`) swaps its value to the atom in place. The UCB lane can promise only that every constant that was an atom at capture is one at install, and its section 10.1 said so in a provisional note addressed to this lane. Revision 3 still had R-UCB-1 ask for constants "atom-backed exactly where the producer's were", and section 17.2 claim that emission read nothing else that changes.

So a body compiled while one of its constants was plain, captured after that constant became an atom, and seeded into a natively decoded consumer UCB, which atomizes every marked constant, installed correctly and then failed the twin check. The twin, compiled against the consumer's atoms, took the fast path where the image kept the generic comparison, and its bytes and fixup list differed from the section's. Constants the consumer's own decode had atomized did the same. The UCB lane's `atom-constants.js` builds exactly these cases.

The revision makes the choice a compile input, with N25 recording the native facts. Unlike the other inputs, it cannot be written into the twin before the compile, because no constant can be made plain again. The templates therefore record the choice where they make it, through the twins-only `ImageRecorder::strictEqualityAtomOperand`, and a twin recorder answers with the producer's choice. Recording at the read was preferred over computing the choice in `snapshotCompileInputs`: it needs no second copy of the templates' selection order and stays exact on whatever thread the producer compiles, and the template edit is needed anyway for the twin side. W2 and W4 check the new kind, W4 including that every operand an input names is an atom string in the consumer, which holds for consistent material by the UCB lane's guarantee. R-UCB-1 now asks for that inclusion and no more, which is all that `UCBConstantAtom` validation and resolution need. I22 states the property and T18 tests it.

### Exported headers (one report)

JSC exports `jit/JIT.h`, `jit/JITMathIC.h`, `jit/AssemblyHelpers.h`, `jit/CCallHelpers.h`, `jit/BaselineJITCode.h` and `assembler/AbstractMacroAssembler.h` in `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`. Bun includes `JavaScriptCore/JIT.h` in `NodeVM.cpp`, `NodeVMScript.cpp`, `NodeVMSyntheticModule.cpp` and `BunJSCModule.h`, and its only JSC include roots are the build's `Headers` and `PrivateHeaders` directories (`scripts/build/deps/webkit.ts`). Revision 3 constructed a `JITCache::MathICRegeneration` by value and called `ImageEmission.h` helpers inside `JITMathIC.h`, and turned inline `AssemblyHelpers.h` functions into callers of helpers that take `ImageReference` by value, while M2 exported no header. Bun would not have compiled. `ImageEmission.h` could not have been included from `AssemblyHelpers.h` in any case: `ImageReference` derives from `CCallHelpers::ConstantMaterializer`, and `CCallHelpers.h` includes `AssemblyHelpers.h`.

The revision takes the first approach the report suggested, which is also the UCB lane's: exported headers gain only forward declarations, opaque enumeration declarations, pointer members of classes whose constructors and destructors are already out of line (`BaselineJITCode`, `JIT`), and declarations of a few hooks with JSC-typed signatures. The hooks take the VM address itself, which `vmAddressTarget` classifies, so no `JITCache` enumeration has to appear in a header, and an inline function calls one only after finding a recorder, so native emission stays inline and unchanged. `vmAddressTarget` now returns an empty optional outside table 8.4, so a hook can make the record unrecordable and fall back to the native sequence. `MathICHolder::forEachMathIC` takes a functor and lets each IC's type give its kind.

`JITMathIC::generateOutOfLine` is defined inside a class template, so its body moves to a new `jit/JITMathIC.cpp` with explicit instantiations for the four specializations the header's typedefs name, the complete set its callers use. Its native callers are the repatching operations in `JITOperations.cpp`, which only need the declaration. The report's other approach, exporting every `jitcache/` header an exported header reaches, was rejected: it would hand the lane's internals to Bun and still leave the `ImageReference` include cycle. Section 15.4 states the rule, I21 the property and T19 the check.

Checking every exported edit against the rule turned up one more hazard the report did not name. A data member of an exported class that existed only under `ENABLE(JITCACHE_TWINS)` would give the class two layouts if the switch reached JSC's translation units and not Bun's, which depends on how the integrator defines it. The only candidate was the twin context `JIT::setJITCacheTwin` had to keep until the compile; the setter now creates the twin recorder in `m_imageRecorder`, which every build has, and section 15.4 forbids twins-only data members in exported classes.

The round's fourth finding, about the counter floor and polymorphic sites after an import, belongs to the CB and UCB lanes and to THREAD; SPEC-cb-history.md and SPEC-ucb-history.md record it.

## Revision 5 (system review round 2, 2026-10-06)

The second system review filed one major against the CB lane that rests on this lane's twin check: the CB corpus required twins to pass while DFG compiles run, and R-INT-11 required `useConcurrentJIT` off in every process of a twins run. The derived runner could satisfy one text at most. The report held up against the code, and the lanes settle it without THREAD.

### Why the check needs `useConcurrentJIT` off

Two facts, one per process. In the producer, `JITWorklist::enqueue` hands the plan to a worklist thread when the option is on, and the LLInt keeps running the CB under compilation, so its `m_modeMetadata`, resolve types and enumerator bytes, and the UCB's arithmetic profiles, can change between `snapshotCompileInputs` and the emitters' reads; the twin compiled from the snapshot can then differ from a correct image. With the option off, `enqueue` compiles and finalizes on the calling thread, which holds the API lock, so no JS of the VM runs during the compile. In the consumer, steps 3 and 5 overwrite the UCB's arithmetic profiles, and compiler threads read them without a lock: `ByteCodeParser::makeSafe` for any CB of the body or for a caller that inlines it, and the MathIC generators of a baseline compile of another CB of the body. A DFG compile that read the overwritten bits would speculate on fewer types than the run had seen, which breaks I20.

### The check guards itself

Making every twins-build run turn the option off would not have been enough: the ICs runner and the CB corpus pass jsc options per run, the option defaults to on, and a check that ran unsound would report differences that are not defects. The check now tests both preconditions before it does anything. `snapshotCompileInputs` records whether the compilation ran on the thread holding the VM's API lock (`VM::currentThreadIsHoldingAPILock`), the twins section carries it as flag bit 1, and a ConsumerProducer's rebuilt record keeps it with the rest of the twin data. `checkImage` skips, with a reason, when that bit is clear or when its own process runs with `useConcurrentJIT` on. The `TwinReport` keeps skips apart from differences, the twins runner fails a twins run on a skip, since its processes all run with the option off, and other runs ignore skips. T2 now requires no skip, T14 states that its imports skip, and I20 names the condition under which the overwrite is safe. The CB, ICs and UCB SPECs follow: their runners fail on differences only, and the UCB lane's `atom-constants.js`, which needs this check to run, turns the option off.

The review's other suggestion, per-lane twin enablement in the integrator's runner, would have left the check unsound wherever a run forgot to disable it, and it would have added a runner switch that no lane needs once the check knows its own preconditions. Recording each input at the emitter's read, as kind 8 already is, would make the producer side exact on a worklist thread, but the consumer side would still need a quiet process, so the gate stays.

## Revision 6 (system review round 3, 2026-10-06)

No finding was filed against this lane. A major against THREAD found that no part owned the executable-allocation fault for DFG and FTL plans, and that section 13 had given the baseline plan's fault to the integrator in an aside, without a requirement or a provisional mark. The finding checks out in `SpeculativeJIT::compile`, `DFG::Plan::finalize` and the callbacks that defer the counters and clear the quick tier-up bits. SPEC-ucb.md R-INT-11 now gives the plan sites of every tier to the integrator, provisionally, since three of the effects they would write are the UCB lane's state. This lane's section 13 row and R-INT-4 point to it, and the lane keeps the MathIC sites of every tier it already edits (section 6.2).

## Revision 7 (five-part system review round 1, 2026-10-06)

The first review of all five SPECs together filed a major against the integrator that rests on this lane's relocation clause: in the twins build, a Debug build with ASan, bmalloc sends every allocation to ASan's allocator, whose space the finding took to sit at a fixed address on x86_64, so the heap domain would never move and every image's `VMAddress::SoftStackLimit` fixup would fail the clause in both the run and its repetition.

The routing to ASan's allocator holds (SPEC-integrator.md N17). The fixed address does not, for the pinned clang 21.1.8 on x86_64: a small ASan program's allocations landed at a different base in each of three fresh processes, position-independent or not, while their distances from each other repeated. Revision 3 had inferred this from the runtime's disassembly; it is now measured, and the table's claim held. The clause did lack a check that the domain moves on the machine and architecture at hand, ARM64 included, which nothing had measured. The integrator's runner now records heap probes, calibrates before its first twins sequence and takes the heap from libpas where the sanitizer's space stays put, and its placement test covers all four domains (SPEC-integrator.harness.md, sections 4, 7.3 and 10). In this lane, the domain table's heap row names what moves the domain and points to that check, the paragraph on coincidences assumes movement only once the runner has checked it, and R-INT-11 states the check and the fallback as requirements on the integrator. SPEC-integrator-history.md, five-part system review round 1, has the full argument.

## Revision 8 (five-part system review round 2, 2026-10-06)

No finding was filed against this lane, and nothing in it changed. Two majors against the CB and ICs lanes found twin checks bundled with tests that need the integrator's runner, while the integrator's install glue waits for every lane's twin check in twins builds. This lane's plan already keeps them apart: task 11 holds `Twins::checkImage` and needs only the integrator's tasks 1 and 2 (the test target, the twin report and the unlimited twin budget), and the runner tests wait in task 12. SPEC-integrator.md section 18 now names task 11 among the twin checks its task 8 waits for.

## Revision 9 (five-part system review round 3, 2026-10-06)

The review filed one major against this lane: the relocation clause fails every import of a body that the importing process captured itself.

### Imports a process captured itself

The finding holds against the SPECs it cites. The integrator's writer adds each commit to the index the process's VMs share, so the committing VM's own lookups see it (SPEC-integrator.container.md, section 8.2, step 8). A ConsumerProducer therefore imports a body it committed when the UCB that produced it dies and is requested again, which `useUnlinkedCodeBlockJettisoning`, Bun's code deletion after the entry script and `Bun.gc(true)` all cause, and it attaches one to a live UCB once the UCB's parked code is released, which SPEC-ucb.md's `live-attach.js` does on purpose. The twins section of such a body holds that process's own resolutions as producer values, so every pair outside the artifact targets is equal, and the integrator's runner fails a run at once on an equal pair in the executable pool or the structure reservation (SPEC-integrator.harness.md, section 7.5). The `ucb/`, `cb/` and `integrator/` directories run their twins sequences with `useConcurrentJIT` off, so the check runs there and these runs failed by construction. A Bun worker importing its main VM's commits meets the same pairs in every domain the two VMs share: the engine image, the structure reservation and the process-wide thunks.

The revision takes the finding's design. Capture writes a 16-byte token of the capturing process into the twins section, drawn once per process and never all zero, and `checkImage` compares no relocation pair for a body whose token is the importing process's own; the byte, fixup, decode, table and rate comparisons still run. The token is per process, not per VM, because the domains that make a same-process import fail (engine image, structure reservation, process-wide thunks) are process-wide. A ConsumerProducer's recapture writes its own token. The rule is provisional (section 17.2): THREAD computes twins with every address differing between producer and consumer, which a same-process import cannot satisfy.

Two alternatives were rejected. Skipping the whole twin check for these imports would lose comparisons that do not depend on moved addresses. Keeping a ConsumerProducer from importing its own commits in twins builds would make those builds behave unlike production. What the clause exists to catch, a reference the record missed, shows only where addresses moved, and every run that imports from another process still runs the clause; T3 now adds a self-import case beside its hook.

## Drain after the thread-prep run (2026-10-06)

THREAD changed after the run, and two agents drained this set against it and against the run's seven open blockers and majors, the second finishing what the first left when its context ran out. The native facts the drain added were read in the code: N26 to N29, the two allocation failures in `JITMathIC::generateOutOfLine`, the repatching operations that call it holding no lock, and `jit/JITCodeMap.h` among the headers JSC exports.

THREAD settles all four provisional choices. Three stand as the SPEC made them: the `SwitchStringRankCase` fixups, now THREAD Capture's one exception to the rule that internal branches carry none; the MathIC snippet fault at both allocation failures of `generateOutOfLine` in every tier, now THREAD Execution's assignment, raised through the integrator's `didFailExecutableAllocation` with `ExecutableAllocationSite::MathICSnippet`; and the relocation clause's exemption for a body the importing process captured itself. The fourth went the other way. THREAD keeps Bun's build flags and has the relocation requirement skip a target inside an object loaded at its link-time address, so no twins build is position-independent, and the clause skips every target that resolves into a loaded segment of an object with a zero load bias: under Bun's flags, the far-call targets and the static atoms. A reference into the engine that a record missed then goes unseen, and it is harmless, since the engine sits at the same address in every process of a build and the restored image holds the right value. The domain table, the repetition rule (now the heap's alone), R-INT-11 and T3 follow, and section 19 now holds the Notes.

The other THREAD changes reached the lane as follows. Strict is off by default and normal mode trusts what the integrity checks cover, so the V, U and W checks join S1 to S5 under strict, `resolveTarget` takes only valid targets, debug builds assert the rest, the tests run with strict on and B1 measures the default. A twin check that skips itself fails a run exactly when every process of that run has `useConcurrentJIT` off. Twins record atom-ness as an input, which revision 4 had already done, and R-UCB-1 now relies on THREAD Restoration's atom contract and nothing more. Section 14 names the four options behind THREAD Storage's "instrumentation that embeds VM addresses". Since a debugger attach turns cache activity off, no recording compilation meets Debugger-mode bytecode, which retires three unrecordable reasons and their census annotations.

The findings ended as follows:

- Blocker, recording after cache activity is off (simplicity): fixed. `producerContext` follows cache activity and gates MathIC regenerations as well, `DebuggerOpcode`, `ShadowChickenOpcode`, `ProfilerOpcode` and `PCToCodeOriginMap` are gone with the option and map checks, `NotShareable` is the one shareability test, and census rows C23 to C25 and T11 follow.
- Major, the arithmetic-profile addend (simplicity): fixed. `m_bits` is the only data member of `ArithProfile` (N26), so the targets carry only the profile index, and census A14 drops the addend.
- Major, a refused charge faulted twice (simplicity): fixed. `recordRebuildRefused` is gone, capture returns `ChargeRefused`, and the budget alone raises the fault.
- Major, per-fixup producer values (simplicity): not adopted, as below.
- Major, no way to enumerate `m_jitCodeMap` (implementability): fixed. Section 15.1 adds `JITCodeMap::size` and `forEach`, capture step 2 reads the map through them, section 15.4 lists the header, and task 9 owns the edit.
- Blocker, rank cases resolved before the string tables hold code addresses (native): fixed. Step 7 of section 11.3 fills the tables before step 8 resolves, and `SwitchStringRankCase` resolves to the location the string table holds for its key (N28), in both processes.
- Major, atom-ness missing from the twin inputs (native): fixed by revision 4's input kind 8 before the drain; THREAD now names the input.

The simplicity review proposed one comparison of domain bases per pair of processes in place of the per-fixup clause. THREAD Verification now words the requirement and both its skips per target, and the clause applies them as written. The proposal compared engine load biases and kept the position-independent twins build; under the Bun flags THREAD keeps, every process of a build has the same bias, so the rule would refuse every pair. The heap has no single base either: cells live in the collector's blocks, while the VM, the atoms and the UCB's profile vectors are malloc allocations, each placed where its allocator's state puts it. The integrator's runner already records a probe of each kind to check that the heap moves at all (SPEC-integrator.harness.md, section 4); the clause then checks the addresses each body names, at the cost of one `u64` per fixup in a section only twins builds write.

## Kept minors from the thread-prep run

- image lane mismatch 1 (section 15.4 said the UCB lane exports Bun's headers): fixed, the integrator's M2 exports its three headers and no lane exports one.
- image lane mismatch 2 (R-INT-11 asked for `Twins` at `start`): fixed, created no later than the first check, as SPEC-integrator.md section 5.1 does, since the class lands with task 11.
- image batch 1.1 support helpers lack a VM: fixed, the four support helpers take `VM&`; every census site holds one.
- image batch 1.2 support helpers lack a VM (repeat): fixed with 1.1.
- image batch 1.3 signatures and undeclared types: fixed, `storeReferenceValue` takes `AssemblyHelpers&`; `StoreValueKind`, `MathICKind`, `BakedFactsBuilder` declared; `VMAddress` and `VMCell` numbered; far calls passed as a span.
- image batch 1.4 factories without a VM, const UCB, unary registers: fixed, UCB factories take `VM&` and `UnlinkedCodeBlock&`, `ResolutionContext::ucb` and `prepareImage` drop const (no const `binaryArithProfile`, `unaryArithProfile`, `hasIdentifier`), section 6.3 gives the unary generator.
- image batch 1.5 named but undefined interfaces: fixed, `ImageRecorder::mathICTarget` for `ImageReference::mathIC`, `MathICRegeneration` returns 0 when inactive; `StringSwitchRanks` already defined; the `ImageCapture` release rule is batch 2's.
- image batch 1.6 `JITCodeMap` cannot list its entries: already fixed, section 15.1 adds `size` and `forEach`.
- image batch 1.7 code-map enumeration and capture's read set: already fixed with 1.6.
- image batch 1.8 code map and the protected ARM64 call offset: fixed, `farCallPointerSite` computes the offset from the public `NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS`; code map already fixed.
- image batch 1.9 private `JIT::m_farCalls`: fixed, `JIT::link` passes `m_farCalls.span()`; the AssemblyHelpers rows were already complete.
- image batch 1.10 far calls and the ARM64 offset: fixed with 1.8 and 1.9.
- image batch 2.1 task 10 needs task 6's `mathICGeneratorFor`: fixed, task 10 now runs after tasks 1, 4 and 6.
- image batch 2.2 task-order errors: fixed, task 10 after task 6; task 7 lands the `negate` operations and T10's stress test first, and T10's live-VM check after task 2, which creates `ImageRecordingTests.cpp`.
- image batch 2.3 two missing dependencies: fixed, task 10 after task 6, and task 9 names `MathICHolder::forEachMathIC`, which S3 uses.
- image batch 2.4 two agents could write the same native code: fixed, task 10 after task 6, and `StringSwitchRecording` moves to task 3 beside the `BinarySwitchRankedComparisons` class it implements.
- image batch 2.5 U checks miss side-table completeness: fixed in part, U2 now requires the key count plus one entries in every string table and U5 one code-map entry per instruction start, with T6 mutations; rejected for `m_ctiDefault`, which nothing reads at run time (`CodeBlock::baselineSwitchJumpTable` has no caller, and a dense switch reaches its default through a direct jump and its `m_ctiOffsets`).
- image batch 2.6 coverage rates and code-map completeness: fixed, V1 bounds both rates to finite values in [0, 1] under strict; the code-map part with 2.5.
- image batch 2.7 nothing releases the `ImageCapture`'s charges: fixed, capture charges before it allocates, a destroyed `ImageCapture` releases what it still holds, and I14 covers it.
- image batch 2.8 capture's buffer and pre-encoded tables: fixed, the writes stream live code spans, canonical footprint bytes from a stack array and table entries encoded as written, since nothing they read changes before JS resumes; the 64 KiB buffer, its charge and its B2 tuning item are gone.
- image batch 2.9 capture's buffer duplicates the writer's: fixed with 2.8.
- image batch 3.1 the inline region holds fixups before its first rewrite in `CannotCompile` bodies: fixed, section 6.2 edit 1 now keeps the fixups past the rewritten bytes valid in dead code and says why they exist (`emitMathICSlow` wires the repatching call when profiling is off, and `generateInline` emits profile writes by default).
- image batch 3.2 same premise, with a T1 case: fixed with 3.1; T1 adds a full inline snippet of a `noDFG` body through its first regeneration.
- image batch 3.3 same premise, in T1 and T5: fixed with 3.1 and 3.2; T5 says that body's profile-write fixups survive the rewrite unchanged.
- image batch 3.4 a partly overlapped footprint is dropped silently: fixed, only footprints wholly inside the rewritten bytes are dropped, and a partial overlap is an `ASSERT` and `Unrecordable(InconsistentRecord)`; no generator's fast path starts with a reference.
- image batch 3.5 the inline-start rewrite needs no recording scope: fixed, `linkJumpToOutOfLineSnippet` stays native with no recorder and `ImageRecord::didRewriteInlineStart(i)` inserts the `SnippetEntry` fixup at the V5 site; `MathICInlineRewrite`, `finishInlineRewrite` and `jumpToSnippet` are gone.
- image batch 3.6 same simplification: fixed with 3.5; `MathICRegeneration::didRewriteInlineStart` replaces `didLinkInlineRewrite`, and the overlap check stays in the record.
- image batch 3.7 the rewrite's attach slot is always empty: fixed with 3.5, `initializeRandom` runs only at the first `random()`, so attach counts are 1 or 2 (section 9.6, W3) and `assemblerSeeds` holds two slots.
- image batch 3.8 census D4 must keep `emit_op_get_from_scope`'s thunk choice: fixed, D4 keys the thunk the native chain links, and the new T20 checks it.
- image batch 3.9 D4 and `GlobalPropertyWithVarInjectionChecks`: fixed with 3.8; `GetFromScopeClosureVarWithVarInjectionChecks` left `BaselineThunk`, since the default branch adds no slow case and no baseline code links that thunk.
- image batch 4.a R-INT-7's rebuilt record only while production is active: fixed, R-INT-7 and install step 14 now match SPEC-integrator.md install step 7, since no capture reads a record once production has ended.
- image batch 4.b heap-oracle test conventions: fixed, section 17.1 now states R-ALL-4's `main(role, scratch, artifact)` rule, role-independent output and heap, the `Off` role without `delta` or JITCache assertions, and `jitcache-heap: off`; T15 calls `delta`, and the oracle compares producers too.
- image batch 4.1 arithmetic-profile byte addend is always zero: already fixed, table 3.3 resolves both kinds to the profile itself (N26).
- image batch 4.2 same addend point: already fixed, as 4.1.
- image batch 4.3 same point with a layout `static_assert`: fixed, N26 now has `ImageRecorder.cpp` assert `sizeof(BinaryArithProfile) == sizeof(BinaryArithProfileBase)` and the unary pair, both `uint16_t`.
- image batch 4.4 step 2 has no outcome with strict off: fixed, normal mode trusts that `JIT::link` put every side-table pointer in the code (THREAD Session), debug builds `ASSERT` it; the unconditional check was rejected, since THREAD leaves assumption checks to strict.
- image batch 4.5 three strict checks duplicate others: fixed for its S4 point with 4.4; S5 was already gone, and S3 checks an assumption, which is strict's job.
- image batch 4.6 encodings repeat the directory and the build ID and encode states that never occur: fixed. The three section headers lose tag, layout, tier and architecture; `m_ctiDefault` is always set, list tables included, and install step 7 fills it; the code map is one offset per instruction start, paired again at install, so U5 checks a count; the twins seed is always drawn by the entry `nop`, and the API-lock flag moved to bit 0. String tables were already full (U2).
- image batch 4.7 section headers repeat the type id and build ID: fixed with 4.6.
- image batch 4.8 duplicate reasons and dead gates: already fixed, only `NotShareable` remains, `isImageCapturable` tests a `Complete` record and section 4.1 has no per-compilation check.
- image batch 4.9 same, plus untestable sampling-flag edits: fixed for the sampling part, the census leaves `setSamplingFlag`, `clearSamplingFlag` and `emitCount` unedited and `ImageRecorder.cpp` has an `#error` for either switch; the rest already fixed.
- image batch 4.10 same, keeping the shareability test: already fixed, a `Complete` record implies shareable code (section 4.7, step 4), which keeps THREAD's condition.
- image batch 5.1 `recordRebuildRefused` doubles the budget's fault: already fixed by the drain, the flag and its R-INT-7 clause are gone, and step 14 and the section 13 row leave the fault to the budget (R-INT-2).
- image batch 5.2 same, plus `RecordPolicy` restating the budget pointer: already fixed for the double route; dropping `RecordPolicy` is cross-set, since the integrator's install steps 7 and 10 pass and test it.
- image batch 5.3 same double route: already fixed with 5.1.
- image batch 5.4 two owners for validation against the UCB: fixed, step 1 of section 11.3 no longer validates; the glue owns validation, under strict and before `compareBakedFacts`, which relies on U4. A validated-view type was rejected: normal mode validates nothing, so every caller would need the view unvalidated anyway.
- image batch 5.5 same single-owner point: fixed with 5.4.
- image batch 5.6 `kMaxIslandHops` = 4 holds only for the default pool: fixed, S1 bounds the chain by the pool's size over half of `nearJumpRange`, rounded up, since `islandForJumpLocation` moves each hop one region of more than half the range and `jitMemoryReservationSize` sets the size freely.
- image batch 5.7 same hop bound: fixed with 5.6; the island-band test it suggests would need allocator internals no header exports.
- image batch 5.8 census E2 names `JIT::compileOpCall`, which this lane does not edit: fixed, E2 and section 15.1 say the two functions take no edit here, their reference being A15's in `CallLinkInfo::emitFastPathImpl`; the code shows no other reference in them.
- image batch 5.9 two SPECs claim `JIT::compileOpCall`: fixed with 5.8, which matches SPEC-ics.md E1's statement that no other part edits it.
- image batch 5.10 same ownership point: fixed with 5.8.
- image batch 6.a `RecordPolicy` restates the budget pointer (cross-set, image side of 5.2): fixed, `RecordPolicy` is gone from section 11.1; `prepareImage` rebuilds the record in step 14 exactly when its budget is non-null, and R-INT-7 asks for a non-null budget exactly in ConsumerProducer VMs whose production is active. The integrator's install steps 7 and 10 change in its own set.
- image batch 6.b a kept twin CB also keeps its body alive (cross-set, from the UCB lane): fixed, section 17.2 step 2 says so; checked that `CodeBlock::visitChildren` always runs `stronglyVisitStrongReferences`, which appends `m_unlinkedCode`, `m_ownerExecutable` and `m_globalObject`, and that step 1 makes the twin from the installed CB's executable, UCB and scope.
- image batch 6.1 fallible steps without an outcome: fixed for the two open parts. `arithProfileTarget` returns nullopt for a profile in neither UCB vector, and a regeneration whose `Complete` record does not list its IC marks the record `Unrecordable(InconsistentRecord)`; both are programming errors that debug builds `ASSERT` (4.1 table, 4.2, section 6.2, census A14). The `didReplaceSlowCall` part was already fixed, and the `ImageCapture` release by batch 2.
- image batch 6.2 R-UCB-1 omits UCB facts the image bakes: fixed, R-UCB-1 now lists the out-of-line jump targets, each function-table entry's builtin, arrow and strict bits and the scope register; checked `JIT::jumpTarget`, `emitNewFuncCommon`, `emitNewFuncExprCommon` and `JIT::emit_op_enter`, and that no other UCB fact reaches the bytes (the other reads feed only the disassembly's debug info). No U check is added: the section records none of these, and the UCB lane's core, which the native `CachedCodeBlock` encoding carries in full, covers them.
- image batch 6.3 same R-UCB-1 point: fixed with 6.2; R-UCB-1 keeps its enumeration, now complete, beside the THREAD Storage guarantee it already cites.
- image batch 6.4 same point for the scope register and the out-of-line targets: fixed with 6.2.
- image batch 6.5 same point, noting section 17.2 already treats the scope register as layout: fixed with 6.2.
- image batch 6.6 R-INT-1 keeps recording after cache activity turns off: already fixed, R-INT-1 makes `producerContext` null for good once cache activity turns off, and the debugger-only reasons are gone from the 4.1 table.
- image batch 6.7 a different "is active" predicate than the UCB lane: already fixed with 6.6.
- image batch 6.8 same predicate: already fixed with 6.6.
- image batch 7.1 N1 misstates how x86_64 code reaches absolute addresses: fixed, N1 now names the two shapes `MacroAssemblerX86_64` emits, a `movabs` into the base register or an `eax` `moffs64` `mov` (`load32`, `load64`, `store32`, `store64` with a `void*`), notes that `memoryModRMAddr` takes a 32-bit address only `loadFromTLS*` and `storeToTLS*` pass, and that R3's helpers replace both shapes; checked in assembler/MacroAssemblerX86_64.h and X86Assembler.h.
- image batch 7.2 same N1 point: fixed with 7.1.
- image batch 7.3 section 11.1's install order leaves out the CB seeds before setup: fixed, the order now puts the CB lane's and the ICs lane's seeds after the other preparations and before `commit` and native setup, as SPEC-integrator.md's install steps 11 to 13 do, and points to those steps for the rest.
- image batch 7.4 same install-order point: fixed with 7.3; R-INT-7 binds the glue to that order and needed no change of its own.
- image batch 7.5 V7 does not check the capability level and taint bytes: fixed, V7 now requires `CannotCompile`, `CanCompile` or `CanCompileAndInline` (the compilation reads `capabilityLevel()`, which memoizes `computeCapabilityLevel` and never returns `CapabilityLevelNotSet`) and a taint of 0 or 1; T6 adds both mutations.
- image batch 7.6 two lanes put the profiler options in different classes: already fixed, SPEC-ucb.md section 12 now adds no row and defers to section 14's fixed rows, and the integrator's table has one class each. Making them free was rejected: THREAD Storage fixes off the instrumentation that embeds VM addresses, and `op_profile_type` and `op_profile_control_flow` embed `TypeLocation` and `BasicBlockLocation` addresses.
- image batch 7.7 records keep step-rounded charges for life: fixed, `finishBaselineCompile` gains step 6, which shrinks the record's vectors and releases every charged byte the record does not hold, `didLinkSnippet` does the same after a regeneration, and step 14 charges a rebuilt record its exact bytes, so a live record's charge follows its bytes rather than the number of live bodies.
- image batch 7.8 records keep a 32-byte entry per reference, and a ConsumerProducer charges a record per import: fixed in part. B2 now measures the charged bytes of live records per body and the rebuilt record's bytes per import, and B5 the rebuilt record at install; the interned-target record is named there as the first change to measure. The compact form itself was rejected for now, since no measurement asks for it yet and THREAD leaves producer memory to the bench.
- image batch 8.1 the twin keep-alive changes what dies in twins runs, so I20 overclaims: fixed. The keep-alive is gone. The twin lives only for its check (a local `Strong`), and a later collection sweeps it. Its destructor skips the `didOptimize` write through a process-wide twin registry that `CodeBlock::~CodeBlock` consults first, a new twins-only edit in section 15.1. I checked `~CodeBlock`: for a CB that never received JIT code, that write is its only effect on shared state, recorded in N18. I20 now says what holds, and step 2 drops the rule that drop and death tests run with concurrent JIT on.
- image batch 8.2 twin CBs pin imported UCBs, executables and realms, and `drop-and-reimport.js` fails: fixed with 8.1, since nothing outlives the check. The UCB lane's workaround of concurrent JIT on is now optional (cross-set).
- image batch 8.3 the same pinning, plus a teardown trap between `Twins` and the UCB registry: fixed with 8.1. The trap was already gone: the integrator destroys the twin-check state in `willDestroyVM` and the registry in `didFinalizeHeap`, and `Twins` now holds nothing, so R-INT-11 no longer constrains when it is destroyed.
- image batch 8.4 the same pinning, plus UFE slots kept full under `useUnlinkedCodeBlockJettisoning`: fixed with 8.1.
- image batch 8.5 the same pinning, proposing to bind the twin CB to a twin UCB: fixed with 8.1. Rejected the twin UCB itself: the twin would resolve UCB targets to its own UCB's addresses, which loses the absolute comparison of UCB targets in the decodes and needs a twin UCB the UCB lane would have to make at install time.
- image batch 8.6 the same pinning, proposing a twins-only hook in `~CodeBlock` that skips the write: fixed as proposed (8.1). The set is process-wide rather than in the VM's state, so the hook reaches it without the integrator's private state, and no exported class gains a data member.
- image batch 8.7 replace the twin CB with a twins-only input source at the metadata reads: the I20 part is fixed with 8.1; the replacement is rejected. It would compile a second baseline artifact against an installed CB, relaxing `RELEASE_ASSERT(!JITCode::isJIT(...))` and requiring a twins-only accessor at every emitter's metadata read, a larger native footprint than one destructor edit.
- image batch 8.8 a failed twin compile leaves the UCB's arithmetic profiles overwritten: fixed. Step 3 saves them in a scope object that restores them on every exit, step 6 says so, and I20 says "restores them on every exit".
- image batch 8.9 the runner-driven test hooks of T3 and T7 have no activation path: fixed. Section 17.1 defines `RelocationPairs`, `OperationPair`, `ChangeRecordedTarget` and `SkipPatch`, section 17.2 declares `ImageTestHook` with `setImageTestHook`, and R-INT-11 asks the integrator for `--jitcache-test-image-hook=<name>` and its Bun variable. Defining a flag for the integrator's own H1 hook is cross-set.
- image batch 9.a file-local names under unified sources (cross-set, SPEC-integrator.md R-ALL-8): fixed, section 15.2 now says the lane's file-local helpers and constants begin with `image` or sit in a per-file named namespace, as SPEC-ics.md section 7 says for `ics`.
- image batch 9.b a UCB that never gets a record still gets charged image records (cross-set, integrator batch 6.10): fixed, section 4.1 records only when the plan's `jitCacheRecordsImage()` is true, a flag R-INT-12 has the integrator compute on the VM thread in `BaselineJITPlan::BaselineJITPlan` from `producerContext(vm)` and `UCBRegistry::keyOf`. Checked that both constructor callers (`jitCompileAndSetHeuristics`, `JIT::compileSync`) run on the VM thread, that `JIT` keeps `m_plan`, and that SPEC-ucb.md records a UCB only at its request points, before publication. Section 1 and the section 15.1 row say the same.
- image batch 9.1 I21 and T19 forbid the jitcache headers other parts export: fixed in scope. The premise is gone (SPEC-ucb.md M2 now exports no header, and the integrator's three exported headers include only WTF and JSC headers), but I21 still stated a rule about headers this lane does not own, stricter than II18. I21 now covers only the exported headers this lane edits and points to II18 for the rest; T19 checks both parts.
- image batch 9.2 same I21 point, plus section 4.2's sentence on every `jitcache/` header: fixed with 9.1; section 4.2 now says "every `jitcache/` header this lane adds".
- image batch 9.3 same I21 point: fixed with 9.1.
- image batch 9.4 same I21 point: fixed with 9.1.
- image batch 9.5 same I21 point: fixed with 9.1.
- image batch 9.6 same I21 point: fixed with 9.1.
- image batch 9.7 same I21 point, with the integrator's exports: fixed with 9.1.
- image batch 9.8 same I21 point, task 12 cannot land: fixed with 9.1; T19 no longer fails on the integrator's exported headers.
- image batch 9.9 same I21 point: fixed with 9.1.
- image batch 9.10 same I21 point, listing an exported set: fixed with 9.1; the exported set stays the integrator's to name (SPEC-integrator.md M2), so I21 lists none.
- image batch 10.1 one-argument `didFailExecutableAllocation` and a contract that differs from the ICs lane's: fixed. The site argument was already in place (R-INT-4 and section 6.2 edit 5 pass `ExecutableAllocationSite::MathICSnippet`). R-INT-4 no longer restates a narrower contract: it gives this lane's call context (API lock held, no `CodeBlock::m_lock`, checked in `operationValueAddProfiledOptimize`) and defers to SPEC-integrator.md section 5.4's contract for every caller, with step `exec-alloc.mathic-snippet`; section 6.2's "no JSC lock held" got the same correction.
- image batch 10.2 same point, with SPEC-ucb.md R-INT-11 quoting R-INT-4: fixed with 10.1; R-INT-4 now points to the integrator's two-argument declaration and contract, which is what R-INT-11's plan sites use.

## Native-fidelity review after the minors (2026-10-07)

- The twin registry's storage lifetime was unspecified (section 17.2, step 2, and the declaration block): fixed. `CodeBlock::~CodeBlock` calls `forgetImageTwin` for every CB in twins builds, and it can run during process exit. `VM::~VM` calls `Heap::lastChanceToFinalize`, which finalizes every live cell; Bun destroys every worker's VM on exit, and the main thread's under `BUN_DESTRUCT_VM_ON_EXIT`; and `process.exit()` calls `exit()`, whose exit-time destructors can run while another thread still finalizes. A set with an exit-time destructor could therefore be gone when a later call reads it. The set is now a function-local `static NeverDestroyed<HashSet<CodeBlock*>>`, created by its first use from whichever thread calls first, which suits a first caller that can be any thread destroying a CB. The lock is a namespace-scope `static Lock imageTwinRegistryLock`, which is constexpr-constructed and has no destructor. `LazyNeverDestroyed` was not chosen because it would need an explicit `construct` that no single first caller can own.

## Compaction (2026-10-07)

SPEC-image.md and SPEC-image.sites.md were shortened without changing what they require. The set keeps both binding files. Every section keeps its number except 11.4, 11.5 and 19, which no longer exist; no section number names different content, though 11.3 now also holds what 11.4 and 11.5 held. Every identifier keeps its name: N1 to N29, R1 to R10, rules C1 to C5, V1 to V7, U1 to U7, W1 to W4, S1 to S5, I1 to I22, R-UCB-1 to R-UCB-4, R-ALL-1, R-ICS-1, R-INT-1 to R-INT-12, M1 to M8, T1 to T21, B1 to B5, tasks 1 to 13, the unrecordable reasons, and census rows A1 to A18 (with A7a, A11a and A15a), B1 to B9, C1 to C27, D1 to D10, E1 to E7 and F1 to F5. Where the right column names a record of this file, that record holds the argument that left the SPEC.

| old (HEAD) | new |
|---|---|
| SPEC-image.md | SPEC-image.md |
| SPEC-image.md, preamble | preamble; THREAD Execution's lane-2 quotation replaced by a citation |
| §1 Scope | §1; the list of what the other parts own replaced by a citation of THREAD Execution |
| §2 Native facts | §2 |
| §3 The reference model; §3.1 to §3.4 | §3; §3.1 to §3.4; R4 cites THREAD Capture instead of quoting it |
| §3.5 String switch ranks | §3.5; THREAD Restoration's and Capture's quotations replaced by citations |
| §4 Recording; §4.1 Who records | §4; §4.1; the callers that run the plan constructor on the VM thread and the cost of recording a UCB without a record are left to "Kept minors from the thread-prep run", batch 9.b |
| §4.2 The recorder | §4.2; its rule on which files include the lane's `jitcache/` headers moved to §15.4 |
| §4.3 Emission helpers | §4.3; why exported headers cannot include `ImageEmission.h` is left to §15.4 and revision 4, "Exported headers" |
| §4.4 Guards | §4.4; its closing sentence now says the twin comparison detects an unguarded pointer immediate only where the producer's and consumer's values differ, citing §17.2's relocation clause |
| §4.5 Veneers | §4.5; the hot-path sentence removed (draft 1, "Why veneers at the end of the allocation") |
| §4.6 The census; §4.7 | §4.6; §4.7 |
| §4.8 Charging | §4.8; why step slack is released is left to "Kept minors from the thread-prep run", batch 7.7 |
| §5 The image record | §5 |
| §6 MathICs; §6.1 Records | §6; §6.1; why the inline-code state is read from the IC is left to revision 1, "MathICs without inline code" |
| §6.2 Regeneration | §6.2; THREAD Execution cited instead of quoted |
| §6.3 Generators; §6.4 The `negate` operations | §6.3; §6.4; THREAD citations instead of quotations |
| §7 Baked facts | §7; THREAD Capture cited instead of quoted |
| §8 Support references; §8.1 to §8.4 | §8; §8.1 to §8.4 |
| §9 Sections; §9.1 to §9.4 | §9; §9.1 to §9.4 |
| §9.5 Validation | §9.5; THREAD Session cited instead of quoted; its closing sentence on W1 to W4 points to §9.6, which states where they run |
| §9.6 `image-twins.baseline` | §9.6 |
| §10 Capture | §10; S1's island-chain argument is left to "Kept minors from the thread-prep run", batch 5.6 |
| §11 Preparation at install; §11.1 Interface | §11; §11.1, which cites I11 instead of restating it |
| §11.2 Context | §11.2 |
| §11.3 Steps | §11.3; step 10 now defines S5, and step 14 leaves the regeneration-log append to §17.2 |
| §11.4 What native setup then does | §11.3, last paragraph |
| §11.5 Strict checks at install (S5) | §11.3, step 10; its sentence on V3 was already step 1 |
| §12 Interfaces; §12.1 | §12; §12.1 |
| §12.2 Requirements on other parts | §12.2; R-UCB-1, R-INT-1, R-INT-4, R-INT-7, R-INT-8 and R-INT-11 cite THREAD instead of quoting or restating it; R-INT-7's reason for a null budget is left to batch 4.a and R-INT-11's reason for creating `Twins` late to mismatch 2, both in "Kept minors from the thread-prep run" |
| §13 Failures | §13; its closing paragraph cites I11 |
| §14 Option table rows | §14 |
| §15 Native edits and owned paths; §15.1 Edits | §15; §15.1, whose row for the JIT emitters cites census E2 instead of restating it |
| §15.2 Owned paths; §15.3 Manifest entries | §15.2; §15.3 |
| §15.4 Exported headers | §15.4, which takes §4.2's include rule |
| §16 Invariants | §16 |
| §17 Tests and bench; §17.1 Test obligations | §17; §17.1; T3 and T7 name the hooks the "Test hooks" paragraph defines |
| §17.2 Twins | §17.2. The paragraph on why randomization alone does not move the pool and the structure reservation is removed (revision 3; the repetition rule stays in R-INT-11 and the domain table's heap row). The paragraph on imports of a body the importing process captured itself is removed (revision 9) except its sentence on forked processes, now in the relocation clause's third skip. The twin registry's exit-order argument is left to "Native-fidelity review after the minors", and the regeneration log's fields to §6.2 |
| §17.3 Bench obligations | §17.3 |
| §18 Tasks | §18 |
| §19 Notes | removed; its one finding not adopted is argued in "Drain after the thread-prep run" |
| SPEC-image.sites.md | SPEC-image.sites.md |
| SPEC-image.sites.md, preamble | preamble, which now says rules C1 to C5 are cited as "rule Cn" |
| sites A to G | A to G, same rows; the notes after B, C, D and F that listed option-only sites, the scope thunk generators and generator literals moved to H |
| sites H, Sites left native | H; its bullets restating rows C23 to C25 replaced by a reference to those rows |
| SPEC-image-history.md | SPEC-image-history.md, with this section appended |

The records above this section cite SPEC-image.md by the section numbers it had when they were written. Where they cite §11.4, §11.5, §19 or "the Notes", the rows for those sections give the new place.

## Walkthrough reports

Two reports from the walkthrough were drained on 2026-10-07: `report/spec-image-pointers-after-compaction.md`, twelve pointers that no longer landed on their rule ("pointers" below), and `report/spec-image-undeclared-interfaces.md`, two interfaces other parts use that the SPEC never declared ("undeclared"). Every item held up against its target and, where it rests on the engine, against the code. None needed THREAD to settle it, and no section or rule number changed.

- pointers 1, `producerContext` after a recording fault (R-INT-1): fixed. THREAD Failures has a recording fault end production and leave cache activity on, and SPEC-integrator.md sections 5.2 and 5.3 return null from `producerContext` once production ends. R-INT-1 now makes the context non-null exactly while production is active, and a recording fault is no longer among the things that turn activity off. Section 4.1, section 10 and R-INT-6 stated capture's condition as cache activity, while the glue requires active production (SPEC-integrator.md sections 4.4 and 9.5), and now say production.
- pointers 2, `SuperSamplerOpcode` named one source of its opcodes: fixed. `Lexer` sets `m_parsingBuiltinFunction` in builtin mode or under `exposePrivateIdentifiers`, and the row now adds that the option stays fixed off, citing SPEC-ucb.md section 12 as it stands.
- pointers 3, section 10 cited R-INT-6 for the replacement and JIT-type conditions: fixed. Section 10 cites SPEC-integrator.md section 9.1, which checks both, and R-INT-6 keeps only when capture may run.
- pointers 4, section 8.3 cited I3, which covered forms only, for valid targets: fixed. I3 now also requires each fixup's fields to meet its kind's strict-check condition in table 3.3. Checked kind by kind: the helpers build each target from the object it names, and the cases that could break a condition (a C++ function outside the text segment, a VM address outside table 8.4, a profile outside both UCB vectors) make the record unrecordable.
- pointers 5, V5 missing from section 8.3's list of target checks: fixed. V5 checks the `ImageOffset` and `SnippetEntry` conditions, as table 3.3 says.
- pointers 6, table 3.3's strict column cited section 9.6: fixed. It cites section 9.5 alone, since no W check reads a fixup target.
- pointers 7, N28 cited for the snippet links: fixed. The table's introduction cites N10 beside N28.
- pointers 8, U5's lookup mode and readers missing from N12: fixed. N12 now says that `JITCodeMap::find` searches with `binarySearch` in mode `KeyMustBePresentInArray`, which asserts in debug builds and in release returns its last candidate, and names the other readers: the `loop_osr` slow path, the two checkpoint exit dispatches in `LLIntSlowPaths.cpp` and `adjustAndJumpToTarget`, which the DFG and FTL exits share.
- pointers 9, `InconsistentRecord` cited too few assertion sites: fixed. The row adds section 6.2 and census A14, where the regeneration and arithmetic-profile cases assert.
- pointers 10, `UnannotatedReference` named shared helpers only: fixed. Census D8, `JIT::emitWriteBarrier(JSCell*)`, is a `JIT` member that nothing calls, so the row now says a path baseline code never takes.
- pointers 11, the comment over the recorder's emission members: fixed. Only the three record functions and `deferConditionalJump` belong to the helpers alone. `emitVeneers`, `noteMathIC`, `bakedFacts`, `markUnrecordable`, `vmAddressTarget` and `arithProfileTarget` also have the call sites of sections 4.5, 6.1 and 7 and the census, and the comment now says so.
- pointers 12, unqualified citations in the census: fixed. A13, B9, D3, D5, the D4 note (I4, T20) and the E note now say SPEC-image, so the E note's R-ALL-1 cannot be read as SPEC-integrator.md's rule of that number.
- undeclared 1, `StringSwitchRecording`: fixed. Section 4.3 declares it with its constructor (the recorder and the table index), the `branch` override and `recordCase(Jump, rank)`, with two rows in the helper table, and section 15.1's prose names `recordCase`. Checked against the code: `BinarySwitch::advance` passes the compared case's position in `m_cases`, which the constructor sorted by signed value, and that position is section 3.5's rank; `caseIndex()` returns the original index, so the leaf's rank needs the new `caseRank()`; and `JIT::emit_op_switch_string` emits the leaf `jump()` after each `advance`, so the JIT calls `recordCase`, not `BinarySwitch`. The class takes the recorder at construction because the JIT creates it only under one. The helper table's mention of a `BinarySwitch` variant of `branchPtrWithReference`, which no declaration has, was dropped.
- undeclared 2, `ImageCheck` and `description(ImageCheck)`: fixed. Section 10 lists the enumerators, V1 to V7, U1 to U7, W1 to W4, S1 to S5 and `None` for the outcomes that name no check (`NotEligible`, `ChargeRefused`, `ExecutableMemoryExhausted`), and declares `ASCIILiteral description(ImageCheck)`, which returns the check's name in lowercase, so `status` reports steps such as `image.v3`. The two failure structs say which checks they carry, and section 15.2 has `ImageSection.cpp` define `description`. The enumerators take the checks' identifiers, where `CBCheck` uses descriptive names, because this SPEC, its failure table and its tests (T6, T7) name every check by identifier.

The walkthrough also asked whether this set states the skip rule as THREAD Verification now words it: a skipped twin check "fails the run exactly when concurrent JIT is off in the process that imports the body and in every process whose captures it imports". It does, and nothing changed. R-INT-11's runner item defers to that rule, and its last item, section 17.2, T2 and T14 state its two directions. SPEC-integrator.harness.md section 7.4 applies it per sequence, to the run and every earlier run that configured JITCache. That set adds earlier Consumer runs, which capture nothing and so cause no image skip. It also covers the process whose compilation a ConsumerProducer's recapture carries: the recapture keeps that compilation's API-lock flag (section 11.3, step 14; I9), so a consumer of the recapture skips when that process compiled concurrently, and the harness then lists the skip. The two agree as long as THREAD's "every process whose captures it imports" counts that process.

T1's closing sentence, that a program reaching the stack limit prints only that the stack-overflow `RangeError` happened, matches THREAD Verification and stays.

## Options table

- Section 14's rows, the facts of its preamble and its free options moved to docs/JitCache/options.md, the one table the five sets share; section 14 points there and keeps `start`'s check after `Config::finalize` and the sentence on CPU features, and sections 4.1, 4.6 (C3), 12.1, R-INT-9 and M4 cite options.md. No option changed class, value or type.

## Runner directives

- R-ALL-4's directives, which SPEC-integrator.harness.md section 7.5 now enforces: T7 declares `image.s1` for its producer with `ChangeRecordedTarget` and `image.s5` for its consumer with `SkipPatch`, T8 `exec-alloc.jitcache-image` and `exec-alloc.mathic-snippet` for the runs its fuzz points fail, and T12 `budget.limit` for its producers; the consumers these faults leave with nothing to install declare `jitcache-expect-no-install`. No test checks anything new.
