# SPEC-image: the Image lane

Lane 2 of THREAD Execution. This file and its one sub-SPEC are binding:

- [SPEC-image.sites.md](SPEC-image.sites.md): the census of every place a baseline image or MathIC snippet embeds a reference, with the helper of section 4.3 each one uses, under the rules C1 to C5 of section 4.6.

[SPEC-image-history.md](SPEC-image-history.md) records why the decisions it names are what they are, for whoever revisits one; nothing in it binds, and implementing needs none of it.

Code is cited by symbol and file. "Native" means the engine as pinned, without JITCache. Every `JITCache` symbol lives in namespace `JSC::JITCache` under `Source/JavaScriptCore/jitcache/`.

## 1. Scope

The lane owns:

1. Recording: the recording scope around every baseline compilation of a UCB that has a record, and every MathIC regeneration of a recorded image, in a producing VM; the fixed-footprint emission of references; the translation of labels to offsets; and the image record that hangs off each `BaselineJITCode`.
2. The image: code bytes, fixups, and everything else `BaselineJITCode` holds: unlinked call records, IC molds (their encoding; the learning state of the ICs built from them is the ICs lane's), switch tables, code map, constant pool, MathICs and the coverage rates `m_livenessRate` and `m_fullnessRate`.
3. Baked facts: what the compilation read from its CB and turned into unguarded code, their section, and the comparison at install.
4. Support references: the keys of the support code and data an image uses, and their resolution through the native lookups.
5. The `negate` operations: `operationArithNegateProfiledOptimize` and the new `operationArithNegateProfiledNoOptimize` (section 6.4).
6. Preparation at install: validation under strict, resolution, allocation, copy, patching and construction of an imported `BaselineJITCode`, up to the point where native setup takes it.
7. The sections `image.baseline` and `baked-facts.baseline`, and in test builds `image-twins.baseline`.
8. The lane's rows in [options.md](../options.md): the fixed options that change the image's bytes or its references, the rows THREAD lists that the lane relies on, and the free options baseline emission reads. `start` checks the fixed ones (R-INT-9).

THREAD Execution gives the rest to the other lanes and the integrator.

## 2. Native facts

- N1. Baseline code makes no PC-relative data reads. No emitter or `MacroAssemblerARM64` path used by baseline emission produces `adr`, `adrp` or a literal load. On x86_64, `MacroAssemblerX86_64` reaches an absolute address in one of two shapes: a 10-byte `movabs` of the address into a register used as the base, or, when the register is `eax`, a `moffs64` `mov` that embeds all eight bytes (`load32`, `load64`, `store32` and `store64` with a `void*` address, through `movl_mEAX`, `movq_mEAX`, `movl_EAXm` and `movq_EAXm`). No operand is RIP-relative, and the SIB absolute form (`X86Assembler::X86InstructionFormatter::memoryModRMAddr`) takes a 32-bit address that only `loadFromTLS*` and `storeToTLS*` pass.
- N2. Every operation call is a far call. `JIT::appendCall` emits `MacroAssembler::call(PtrTag)`: on x86_64 `moveWithPatch(TrustedImmPtr(nullptr), r11)` then `call r11`; on ARM64 `moveWithFixedWidth` into the data temp then `blr`. `JIT::link` fills each placeholder from `JIT::m_farCalls` through `LinkBuffer::link`, which writes the pointer with `Assembler::linkPointer` at the call label minus `REPATCH_OFFSET_CALL_R11` (x86_64) or plus `REPATCH_OFFSET_CALL_TO_POINTER` (ARM64). `m_farCalls` dies with the `JIT` object.
- N3. Three paths call C++ through an unrecorded pointer immediate: `AssemblyHelpers::callExceptionFuzz`, which `AssemblyHelpers::emitExceptionCheck` emits under `useExceptionFuzz`; `MacroAssembler::probeDebug`, which `JIT::privateCompileMainPass` and `JIT::privateCompileSlowCases` emit under `traceBaselineJITExecution`; and `CCallHelpers::ensureShadowChickenPacket` in `JIT::emit_op_log_shadow_chicken_prologue` and `JIT::emit_op_log_shadow_chicken_tail`.
- N4. Data pointer immediates have no native record: `JIT::emit_op_switch_imm` and `JIT::emit_op_switch_char` move the dense table's `m_ctiOffsets` base into a register.
- N5. Thunk links. `MacroAssembler::nearCallThunk`, `nearTailCallThunk` and `jumpThunk`, and `Call::linkThunk` and `Jump::linkThunk` in `AbstractMacroAssembler.h`, link on x86_64 through a `LinkBuffer` link task (a `rel32`), and on ARM64 through an `ARM64Assembler::LinkRecord` marked as a thunk, which `LinkBuffer::copyCompactAndLinkCode` links after compaction. A conditional thunk link is compacted by distance to the thunk's absolute address (`ARM64Assembler::computeJumpType`), so its bytes depend on where image and thunk landed.
- N6. On ARM64, `move(TrustedImmPtr)` uses `moveInternal`, whose instruction count depends on the value; `moveWithPatch` and `moveWithFixedWidth` always emit `NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS` (3 on Linux ARM64) `movz`/`movk`. Absolute-address loads and stores go through the cached memory temp with value-relative reuse (`MacroAssemblerARM64::load`, `store`, `tryMoveUsingCacheRegisterContents`), and `or64(TrustedImm64, ...)` uses one `orr` when the value is a valid logical immediate.
- N7. On x86_64, `move(TrustedImmPtr)` emits a 10-byte `movabs` for any nonzero value; `branch64` and `store64` with a `TrustedImm64` fold to an `imm32` form when the value fits; `ImmPtr` and `Imm64` may be blinded.
- N8. `structureIDBase()` enters code only through `AssemblyHelpers::emitNonNullDecodeZeroExtendedStructureID`, as `or64(TrustedImm64(structureIDBase()), source, dest)`.
- N9. An inline `switch_string` (`JIT::emit_op_switch_string`) builds a `BinarySwitch` of type `IntPtr` over its case atoms' addresses. `BinarySwitch::build` sorts the cases by signed value, and the tree it emits depends only on the case count and on draws from `m_weakRandom`, seeded per switch from a file-static counter; `allConsecutive` needs keys one apart, which distinct heap objects never are. Comparisons use `ImmPtr`. Each leaf returns to the JIT, which emits `jump()` and adds it to `m_jmpTable` for the case's bytecode target.
- N10. `JITMathIC::generateOutOfLine` (`JITMathIC.h`) builds a snippet in its own `CCallHelpers`, links it back to the image with `jumpThunk(doneLocation())` and `linkThunk(slowPathStartLocation())` (the latter on conditional jumps), allocates it with `JITCompilationCanFail`, repoints the slow call with `ftlThunkAwareRepatchCall`, and rewrites the inline start with a jump through a rewriting `LinkBuffer` (`linkJumpToOutOfLineSnippet`).
- N11. `operationArithNegateProfiledOptimize` passes `operationArithNegateProfiled`, which takes a `UnaryArithProfile*`, as the replacement call, while the call site's argument is the IC's address (`JIT::emitMathICSlow`). Add, sub and mul pass `*ProfiledNoOptimize` operations that take the IC and read `arithProfile()`.
- N12. `CodeBlock::setupWithUnlinkedBaselineCode` reads the molds, the constant pool, `m_jitCodeMap` (exception handlers, through `JITCodeMap::find`) and `m_isShareable`, and checks none of them. `JITCodeMap::find` searches with WTF's `binarySearch`, whose mode is `KeyMustBePresentInArray`: a missing key fails an assertion in debug builds and in release builds yields the search's last candidate, another instruction's address, instead of null. The map's other readers look instruction starts up through the same `find`: the LLInt's loop OSR entry (the `loop_osr` slow path), the exits that resume after a checkpoint (`dispatchToNextInstructionDuringExit`, `dispatchToCurrentInstructionDuringExit`) and the DFG and FTL OSR exits that land in baseline code (`adjustAndJumpToTarget`).
- N13. The compiled code depends on the capability class only through `CannotCompile` versus the rest (`JIT::compileAndLinkWithoutFinalizing`). `CodeBlock::computeCapabilityLevel` writes nothing; `CodeBlock::capabilityLevel` memoizes into `m_capabilityLevelState`.
- N14. The assembler's random source is a `WeakRandom` seeded from a process counter that starts at one cryptographic draw (`AbstractMacroAssemblerBase::initializeRandom`).
- N15. A `BaselineJITCode` gets a `PCToCodeOriginMap` only when `VM::shouldBuilderPCToCodeOriginMapping()` holds. `VM::setShouldBuildPCToCodeOriginMapping` has three callers: `VM::VM` under `useSamplingProfiler` and `alwaysGeneratePCToCodeOriginMap`, and `Debugger::attach`. Bun's runtime CPU profilers call `VM::ensureSamplingProfiler`, which does not call it.
- N16. Every piece of VM data a recorded image references exists from VM construction: the fields behind `VM::addressOfSoftStackLimit`, `VMTraps::trapBitsAddress`, `VM::addressOfException`, `VM::topEntryFrame`, `VM::targetMachinePCForThrow`, `VM::topCallFrame`, `VM::addressOfMightBeExecutingTaintedCode`, `Heap::addressOfBarrierThreshold`, `Heap::addressOfMutatorShouldBeFenced`, the `UniqueRef` behind `VM::syncResumeCallCache`, `jsEmptyString` and `SmallStrings::sentinelString`.
- N17. `JIT::emitMathICFast` calls `JITMathIC::generateInline`, which returns false in two cases:
  - `DontGenerate`, when the profile saw only non-numbers on both sides (the `generateInline` of `JITAddGenerator`, `JITSubGenerator` and `JITMulGenerator`) or on the operand (`JITNegGenerator::generateInline`), as at a `+` that only saw strings or a `*` that only saw BigInts;
  - `GenerateFullSnippet` followed by a `generateFastPath` that refuses, before emitting anything, because the bytecode's `OperandTypes` say an operand cannot be a number (`JITAddGenerator::generateFastPath`, `JITMulGenerator::generateFastPath`), as at `"x" + y`.

  In both cases `emitMathICFast` calls the operation directly (census F2) and adds no slow case. `JIT::privateCompileSlowCases` walks only `m_slowCases`, so `emitSlow_op_*` never runs for that bytecode and neither does `JIT::emitMathICSlow`, the only place that registers the `finalizeInlineCode` link task. The IC keeps null `m_inlineStart`, `m_inlineEnd`, `m_slowPathStartLocation` and `m_slowPathCallLocation`, no `m_code` and `m_generateFastPathOnRepatch` false, and with no slow call nothing ever reaches its `generateOutOfLine`. Whenever `generateInline` returns true, every path appends at least one slow-path jump (the empty profile's `patchableJump`, or an overflow or type branch), so the IC gets all four locations.
- N18. `ScriptExecutable::newCodeBlockFor` `RELEASE_ASSERT`s that the executable has no CB of the requested kind, and the `CopyParsedBlock` constructor of `CodeBlock` shares the other CB's `m_metadata`. `CodeBlock::~CodeBlock` sets `didOptimize` in the UCB's unlinked metadata to `False` while it is indeterminate, unless `Heap::isShuttingDown()`, which `Heap::lastChanceToFinalize` sets after `VM::~VM` has deferred GC for good. For a CB that never received JIT code, that write is the destructor's only effect on state other code reads: it stamps `m_ownerWentAwayAt` only for `BaselineJIT` code, frees `catch` buffers only for baseline code, and finds no incoming call (`unlinkOrUpgradeIncomingCalls`) and no IC.
- N19. `AbstractMacroAssemblerBase` seeds its `WeakRandom` lazily, at the first `random()`, while `BinarySwitch` seeds `m_weakRandom` from its own file-static counter and draws inside its constructor as it builds the tree.
- N20. A `LinkBuffer` writes exactly `LinkBuffer::size()` bytes of its allocation, and the allocation can be larger. On x86_64, `LinkBuffer::linkCode` copies the whole assembler buffer, which `LinkBuffer::allocate` padded with breakpoints to `jitAllocationGranule`. On ARM64, `LinkBuffer::copyCompactAndLinkCode` sets `m_size` to the compacted size, calls `shrink` on the handle and copies `m_size` bytes. `MetaAllocatorHandle::shrink` rounds the new size up to the allocator's granule, and with the libpas JIT heap `ExecutableMemoryHandle::createImpl` and `ExecutableMemoryHandle::shrink` take `jit_heap_get_size`, a size-class bound. The bytes past `size()` keep whatever the pool held before. `JITCodeWithCodeRef::size` and `MacroAssemblerCodeRef::size` return the handle's `sizeInBytes()`, and `JIT::finalizeOnMainThread` samples that size.
- N21. The link writers act on whatever their site holds. `X86Assembler::linkJump`, `linkCall` and `linkPointer` store the field before the site (`setRel32`, `setPointer`) without reading the opcode. `ARM64Assembler::linkJump` calls `relinkJumpOrCall<BranchType_JMP>`, which, when the site holds a `nop` (`disassembleNop`), decodes the instruction before the site as a conditional, compare or test branch and rewrites both instructions through `linkConditionalBranch`, `linkCompareAndBranch` or `linkTestAndBranch`. `ARM64Assembler::linkPointer` takes `Xd` from the `movz` at the site and writes three instructions with it (`setPointer`).
- N22. `JSC::initialize` makes both process-wide reservations once, before any VM exists (`InitializeThreading.cpp`). `ExecutableAllocator::initialize` reserves the executable pool, bounded by `g_jscConfig.startExecutableMemory` and `endExecutableMemory`, at `Options::jitMemoryReservationAddress()` when that restricted option is set, with a `RELEASE_ASSERT` that the base equals it (`initializeJITPageReservation`); the jsc shell enables restricted options and parses its command line before calling `JSC::initialize` (`jscmain`). `StructureAlignedMemoryAllocator::initializeStructureAddressSpace` constructs `StructureMemoryManager`, which reserves the structure heap aligned to its own size, 4 GiB on Linux (`computePreferredStructureHeapReservationSize`, `OSAllocator::tryReserveUncommittedAligned`), records it in `g_jscConfig.startOfStructureHeap` and `sizeOfStructureHeap`, and derives `structureIDBase` from its start. With `useJIT` on, `LLInt::defaultCall()` and `LLInt::arityFixup()` return process-wide thunks linked into the pool (`defaultCallThunk` and `arityFixupThunk` in `LLIntThunks.cpp`).
- N23. Bun's Linux builds compile and link the engine position-dependent. In Bun's `scripts/build/flags.ts`, `bunOnlyFlags` passes `-fno-pic -fno-pie` to Bun's own compiles and `linkerFlags` passes `-fno-pic -Wl,-no-pie` to the Linux link ("No PIE (we don't need ASLR; simpler codegen)"). The local WebKit recipe that `bun build.ts` runs (`scripts/build/deps/webkit.ts`) puts `-fno-pic -fno-pie -no-pie` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`, which CMake also passes to the link, and turns `CMAKE_POSITION_INDEPENDENT_CODE` off for its `jsc` target. The engine image, its code and its static data, therefore sits at the same address in every process of one build, and its object has a zero load bias.
- N24. Some atoms are static data of the engine image. `StringImpl::empty()` returns the static `StringImpl::s_emptyAtomString`, which `JSString::createEmptyString` makes the value of `vm.smallStrings.emptyString()`; `BytecodeGenerator::addStringConstant` builds every string constant with `jsString`, which returns that cell for an empty string, so the `tryGetValueImpl()` of every `""` constant is the static object. `AtomStringImpl::add(const StaticStringImpl&)` stores a static `StringImpl` built as an atom directly in the atom table (`addStatic`).
- N25. The strict-equality templates choose their shape from atom-ness, which changes. `JIT::compileOpStrictEq` (`stricteq`, `nstricteq`) and `JIT::compileOpStrictEqJump` (`jstricteq`, `jnstricteq`) first look for an operand that is a constant the UCB owns holding `undefined`, `null` or a boolean (`tryGetBitwiseComparableConstant`). When neither is, each asks its `tryGetAtomStringConstant` lambda about the left operand and then the right: a constant the UCB owns (`CodeBlock::isConstantOwnedByUnlinkedCodeBlock`, which reads only the UCB) that is a string whose `tryGetValueImpl()` exists and `isAtom()`. The first operand found gets a fast path that compares the other operand's `StringImpl` with that atom's address; with none, the template emits the generic comparison. The slow paths do not ask again (`DEFINE_SLOWCASE_SLOW_OP` for `stricteq` and `nstricteq`, `operationCompareStrictEq` in `emitSlow_op_jstricteq` and `emitSlow_op_jnstricteq`), so the choice is made once per instruction, in the main pass. A constant's atom-ness only grows (SPEC-ucb.md, F19): a natively decoded constant becomes an atom in place when a property-key use or the decoder's string table promotes it, so two compilations of one body can choose differently, and a compilation can choose the generic comparison for a constant that is an atom by the time the body is captured (compiler.md, "Whether a string constant is an atom also changes over time").
- N26. `ArithProfile` has one data member, `m_bits`, and no virtual function, and `UnaryArithProfile` and `BinaryArithProfile` add no data member (`bytecode/ArithProfile.h`), so `addressOfBits()` is the profile's own address.
- N27. `JITCodeMap` exposes only its constructors, `find` and `operator bool`; its entries and their count are private (`jit/JITCodeMap.h`, a header JSC exports, section 14.4). `JIT::link` builds it from `m_labels` through `JITCodeMapBuilder`.
- N28. `JIT::link` fills a string table's `m_ctiOffsets[m_indexInTable]` with the code location of each key's branch target and the entry after the last key with the default (`SwitchRecord::String`). An inline `switch_string`'s leaf for a key jumps to that same location (`JIT::emit_op_switch_string`), since every key's branch offset is nonzero: `CaseBlockNode::emitBytecodeForBlock` emits each clause label after the switch instruction, and `BytecodeGenerator::endSwitch` binds the labels relative to it.
- N29. Only the profiler opcodes clear `m_isShareable` (`JIT::emit_op_profile_type`, `JIT::emit_op_profile_control_flow`), and Bun turns the control-flow profiler on at run time for coverage, so a producing VM can compile them. `op_debug` and the ShadowChicken opcodes exist only in Debugger-mode bytecode (`BytecodeGenerator::emitDebugHook`, `emitLogShadowChickenPrologueIfNecessary` and `emitLogShadowChickenTailIfNecessary` test `shouldEmitDebugHooks()`, or the fixed-off `alwaysUseShadowChicken`), which needs an attached interactive debugger or options THREAD fixes off (`JSGlobalObject::defaultCodeGenerationMode`). `VM::m_perBytecodeProfiler` is created only in `VM::VM`, under `useProfiler`.

## 3. The reference model

### 3.1 Terms

The image is the code `LinkBuffer` wrote for one `BaselineJITCode`: its executable allocation from the normal entry (offset 0) up to `codeSize`, the linked size `LinkBuffer::size()` that `JIT::link` reads. A snippet is the code a MathIC regeneration's `LinkBuffer` wrote into the allocation `JITMathIC::m_code` holds, up to that buffer's `size()`, its `snippetSize`. The bytes of an allocation past its linked size belong to neither: no `LinkBuffer` writes them, so they hold whatever the pool held (N20), and capture, import and the twin check never read them. A reference is a byte range of an image or snippet whose value depends on the process, the VM, a UCB, or another allocation. A literal is any other immediate. An internal branch is a branch whose source and target both lie in the same allocation.

Each reference is a fixup: a site, a form and a typed target. Recording emits every reference in its form's fixed footprint, unblinded and unfolded, and records the fixup. Literals keep native emission, blinding included.

### 3.2 Forms

The site is the offset the assembler's own link writer takes, so the consumer patches with that writer and nothing else.

| form | x86_64 footprint and site | ARM64 footprint and site | writer (x86_64 / ARM64) |
|---|---|---|---|
| `Pointer` | `movabs r64, imm64` (`REX.W B8+r`, 10 bytes); site is the end of the immediate; footprint `[site-10, site)` | `movz Xd,#h0` ; `movk Xd,#h1,lsl#16` ; `movk Xd,#h2,lsl#32` (12 bytes, same `Xd`); site is the first instruction; footprint `[site, site+12)` | `X86Assembler::linkPointer` / `ARM64Assembler::linkPointer` |
| `Call` | `call rel32` (`E8`); site is the end of the `rel32`; footprint `[site-5, site)` | `bl`; site is the end of the `bl`; footprint `[site-4, site)` | `X86Assembler::linkCall` / `ARM64Assembler::linkCall` |
| `Jump` | `jmp rel32` (`E9`) or `jcc rel32` (`0F 80+cc`); site is the end of the `rel32`; footprint `[site-5, site)` when `site-5` holds `E9`, `[site-6, site)` when `site-6` holds `0F 80` to `0F 8F` | `b`; site is the instruction; footprint `[site, site+4)` | `X86Assembler::linkJump` / `ARM64Assembler::linkJump` |

On ARM64 a `Jump` fixup is always an unconditional `b`; recording never leaves a conditional branch whose target lies outside its allocation (rule R6). The ARM64 writers place jump islands themselves when a target lies beyond `b` or `bl` range (`ARM64Assembler::linkJumpOrCall`).

Fixups are kept in footprint order: by site, and at a shared site the `Call` first, so each footprint starts at or after the end of the one before it. On x86_64 every site ends its footprint, so no two fixups share a site. On ARM64 a `Call`'s site ends its footprint while a `Pointer`'s or a `Jump`'s site starts it, so a `bl` followed at once by a recorded `movz` or `b` puts two fixups at one site. A site and a form name at most one fixup. History: [Fixup order](SPEC-image-history.md#fixup-order).

A footprint in canonical encoding is exactly these bytes, where `r` is the register's low three bits and `d` one register number shared by the three words. Each form's variable field is zero and every other bit is kept:

| form | x86_64 | ARM64 |
|---|---|---|
| `Pointer` | `48` or `49`, then `B8+r`, then eight `00`: the 64-bit immediate is zero | words `D2800000\|d`, `F2A00000\|d`, `F2C00000\|d`: the `imm16` field (bits 5 to 20) of each word is zero, opcodes and `Xd` kept |
| `Call` | `E8 00 00 00 00`: the `rel32` is zero | word `94000000`: the `imm26` field (bits 0 to 25) is zero |
| `Jump` | `E9 00 00 00 00`, or `0F`, one byte from `80` to `8F`, `00 00 00 00`: the `rel32` is zero | word `14000000`: the `imm26` field is zero |

The writers trust their site (N21): an ARM64 `linkJump` that met a `nop` would rewrite the instruction before its footprint, before the allocation at site 0. Capture therefore writes every footprint in its canonical encoding (section 8.4, I8), and section 8.5 says what checks it. History: [Forms and canonical footprints](SPEC-image-history.md#forms-and-canonical-footprints).

### 3.3 Target kinds

A target is `ImageTarget { TargetKind kind; uint32_t a; uint32_t b; int64_t payload; }`. The producer's helpers compute every value they emit, and the consumer resolves every target, through one function, `resolveTarget` (section 3.8). The three kinds that native linking resolves at emission, `SwitchStringRankCase`, `ImageOffset` and `SnippetEntry`, resolve there to the location linking wrote, read from the same tables and allocations (N10, N28).

| kind | fields | forms | value | strict check (section 8.5) |
|---|---|---|---|---|
| `Operation` | `payload` = `CodeSymbol` | `Pointer` | the C++ function's address | symbol inside the engine's text segment, instruction-aligned |
| `CommonThunk` | `a` = `CommonJITThunkID` | `Call`, `Jump` | `VM::getCTIStub(id)` | `a < numberOfCommonThunkIDs` |
| `BaselineThunk` | `a` = `BaselineThunk` | `Call`, `Jump` | `VM::getCTIStub(JIT::baselineThunkGenerator(a))` | `a` in range |
| `SlowPathThunk` | `payload` = `CodeSymbol` of a `SlowPathFunction` | `Call` | `JITThunks::ctiSlowPathFunctionStub(vm, function)` | as `Operation` |
| `InlineCacheSlowPathThunk` | `a` = `AccessType` | `Call` | `InlineCacheCompiler::generateSlowPathCode(vm, type)` | `a` a valid `AccessType` |
| `VirtualCallThunk` | `a` = `CallMode` | `Call` | `VM::getCTIVirtualCall(mode)` | `a` a valid `CallMode` |
| `ProcessThunk` | `a` = `ProcessThunk` | `DefaultCall`: `Pointer`; `ArityFixup`: `Call` | `LLInt::defaultCall()`, `LLInt::arityFixup()` | `a` in range, form matches |
| `VMAddress` | `a` = `VMAddress` | `Pointer` | the address in table 3.6 | `a` in range |
| `VMCell` | `a` = `VMCell` | `Pointer` | `jsEmptyString(vm)` or `vm.smallStrings.sentinelString()` | `a` in range |
| `StructureIDBase` | none | `Pointer` | `structureIDBase()` | none |
| `UCBConstantCell` | `a` = constant index | `Pointer` | the cell of the UCB's constant `a` | `a` below the UCB's constant count; the UCB owns it (representation `Other`, a cell, neither `SymbolTable` nor `JSTemplateObjectDescriptor`, as `CodeBlock::isConstantOwnedByUnlinkedCodeBlock` decides) |
| `UCBConstantAtom` | `a` = constant index | `Pointer` | `asString(constant)->tryGetValueImpl()` | constant `a` is a `JSString` whose value impl exists and is an atom |
| `UCBIdentifier` | `a` = identifier index | `Pointer` | `ucb.identifier(a).impl()` | `a` below the identifier count |
| `UCBBinaryArithProfile` | `a` = profile index | `Pointer` | `&ucb.binaryArithProfile(a)`, which is also its `addressOfBits()` (N26) | `a` below the count |
| `UCBUnaryArithProfile` | as above, unary | `Pointer` | `&ucb.unaryArithProfile(a)` | as above |
| `SwitchStringRankAtom` | `a` = string switch table, `b` = rank | `Pointer` | the key of rank `b` in the consumer's table `a` (section 3.7) | `a` below the string table count, `b` below its key count, the count at most `maximumInlineStringSwitchCaseCount` |
| `SwitchStringRankCase` | as above | `Jump` (internal) | the code location the image's string table `a` holds for the key of rank `b` (section 3.7) | as above |
| `MathIC` | `a` = MathIC index | `Pointer` | the address of the image's MathIC `a` | `a` below the MathIC count, naming a MathIC with inline code (section 6.1) |
| `SwitchTableBase` | `a` = simple switch table | `Pointer` | `m_switchJumpTables[a].m_ctiOffsets` storage | table `a` is dense (entry count above 0) |
| `ImageOffset` | `a` = image offset | `Jump`, in snippets only | image start plus `a` | `a` is the owning MathIC's `inlineEnd` or `slowPathStart` |
| `SnippetEntry` | `a` = MathIC index | `Jump`, in the image only | the start of MathIC `a`'s snippet | MathIC `a` has a snippet and the fixup sits at its inline start (section 8.5, V5) |

Unused fields are zero, and strict rejects a nonzero unused field.

`ImageRecorder.cpp` pins N26 at build time with `static_assert(sizeof(BinaryArithProfile) == sizeof(BinaryArithProfileBase))` and the same for `UnaryArithProfile` and `UnaryArithProfileBase`, so an engine change that adds a member fails to build instead of shifting every profile target.

### 3.4 Support keys

Support code is the code an image calls that the consumer generates or finds natively; its key is the native lookup's key with process-local parts replaced by build-stable symbols (THREAD Restoration). The support kinds of table 3.3 are `Operation`, `CommonThunk`, `BaselineThunk`, `SlowPathThunk`, `InlineCacheSlowPathThunk`, `VirtualCallThunk` and `ProcessThunk`.

```cpp
namespace JSC::JITCache {
enum class BaselineThunk : uint8_t {
    OpEnterHandler = 1, OpCheckTrapsHandler, OpThrowHandler, ValueIsTruthy, ValueIsFalsey, SlowOpPutToScope,
    ResolveScopeClosureVarWithVarInjectionChecks, ResolveScopeGlobalVar, ResolveScopeGlobalProperty, ResolveScopeGlobalLexicalVar,
    ResolveScopeGlobalVarWithVarInjectionChecks, ResolveScopeGlobalPropertyWithVarInjectionChecks, ResolveScopeGlobalLexicalVarWithVarInjectionChecks,
    GetFromScopeGlobalVar, GetFromScopeGlobalProperty, GetFromScopeGlobalLexicalVar, // no baseline code links the ClosureVarWithVarInjectionChecks thunk (census D4)
    GetFromScopeGlobalVarWithVarInjectionChecks, GetFromScopeGlobalLexicalVarWithVarInjectionChecks,
    ConsistencyCheck, // ASSERT_ENABLED builds
};
enum class ProcessThunk : uint8_t { DefaultCall = 1, ArityFixup = 2 };
}
```

`JIT::baselineThunkGenerator(JITCache::BaselineThunk) -> ThunkGenerator`, a new public static member declared in `JIT.h`, maps each entry to the private generator it names (`JIT::op_enter_handlerGenerator`, `JIT::generateOpResolveScopeThunk<GlobalVar>`, and so on). It is defined in `JITPropertyAccess.cpp`, the one file that defines the member templates `generateOpResolveScopeThunk` and `generateOpGetFromScopeThunk`, because once the census routes the scope emitters through this mapping it alone names their twelve specializations, and only a file that sees a template's definition instantiates them. The JIT's own emitters call the helpers with these keys, so producer and consumer go through one mapping. A thunk generator is code JITCache would invoke at import, so its key comes from this closed enum and never from a symbol in the file. History: [Code symbols and the thunk enumeration](SPEC-image-history.md#code-symbols-and-the-thunk-enumeration).

### 3.5 Code symbols

C++ functions (operations and slow-path functions) are keyed by a `CodeSymbol`: the function's address minus the address of `JITCache::codeSymbolAnchor`, a function the module defines in the engine. The header's build ID covers the object containing the anchor (THREAD Storage; R-INT-8), so equal builds give equal offsets.

```cpp
namespace JSC::JITCache {
struct CodeSymbol {
    int64_t offset;
    static std::optional<CodeSymbol> of(const void* function); // nullopt outside the engine's text segment
    const void* address() const;                               // RELEASE_ASSERTs isValid
    static bool isValid(int64_t offset);                       // inside the text segment, instruction-aligned
};
}
```

The text segment is the executable `PT_LOAD` segment of the object containing the anchor, found once per process with `dladdr` and `dl_iterate_phdr` and cached. A far call whose callee lies outside it makes the compilation `Unrecordable(ForeignCodeSymbol)`. A slow-path function is only passed as data to `JITThunks::ctiSlowPathFunctionStub`, which embeds it in a generated thunk, so a `CodeSymbol` is enough for it too. History: [Code symbols and the thunk enumeration](SPEC-image-history.md#code-symbols-and-the-thunk-enumeration).

### 3.6 VM data

`enum class VMAddress : uint8_t` and `enum class VMCell : uint8_t` number their entries from 1 in the order of the two tables below (`VMAddress::VM` = 1 through `SyncResumeCallCache` = 11, `VMCell::EmptyString` = 1 and `SmallStringsSentinel` = 2), which is the `a` a fixup carries (section 8.2).

| `VMAddress` | value |
|---|---|
| `VM` | `&vm` |
| `SoftStackLimit` | `vm.addressOfSoftStackLimit()` |
| `TrapBits` | `vm.traps().trapBitsAddress()` |
| `Exception` | `vm.addressOfException()` |
| `TopEntryFrame` | `&vm.topEntryFrame` |
| `TargetMachinePCForThrow` | `&vm.targetMachinePCForThrow` |
| `TopCallFrame` | `&vm.topCallFrame` (debug builds only reference it) |
| `MightBeExecutingTaintedCode` | `vm.addressOfMightBeExecutingTaintedCode()` |
| `BarrierThreshold` | `vm.heap.addressOfBarrierThreshold()` |
| `MutatorShouldBeFenced` | `vm.heap.addressOfMutatorShouldBeFenced()` (ARM64 code only references it) |
| `SyncResumeCallCache` | `&vm.syncResumeCallCache()` |

| `VMCell` | value |
|---|---|
| `EmptyString` | `jsEmptyString(vm)` |
| `SmallStringsSentinel` | `vm.smallStrings.sentinelString()` |

Every entry exists from VM construction (N16) and outlives every image of the VM. The tables are closed: images avoid VM data that exists lazily or only under an option (THREAD Restoration), and a reference to other VM data has no target kind, so its compilation is unrecordable (section 4.1).

### 3.7 String switch ranks

String switches keep the producer's tree, and each rank's case jump carries a fixup (THREAD Restoration and Capture). For an inline `switch_string` over table `t` with `n` keys, rank `r` is the position of a key in the table's keys sorted by signed address (the order `BinarySwitch` sorts in, N9). Recording gives every comparison against rank `r`'s key a `Pointer` fixup with target `SwitchStringRankAtom(t, r)`, and the leaf `jump()` that executes rank `r` a `Jump` fixup with target `SwitchStringRankCase(t, r)`, the only internal branch that carries one.

`StringSwitchRanks` (section 3.8) holds, for each string table that emission gives an inline tree, its keys sorted by signed address, each with its `m_indexInTable`; the producer builds it from its UCB's tables at capture and the consumer from its own at import. Rank `r` then names key `k(r)`. `SwitchStringRankAtom(t, r)` resolves to `k(r)`, and `SwitchStringRankCase(t, r)` to `m_stringSwitchJumpTables[t].m_ctiOffsets[m_indexInTable of k(r)]`, which holds the code for that key's branch target (N28): in the producer `JIT::link` filled it, and the consumer fills it before it resolves any target (section 10.3, step 7). The tree's shape depends on `n` and the draws alone (N9), so it is valid for any order of addresses. History: [String switch ranks](SPEC-image-history.md#string-switch-ranks).

### 3.8 Resolution

```cpp
namespace JSC::JITCache {
struct StringSwitchRank {
    UniquedStringImpl* key;  // a key of the UCB's unlinked string table
    unsigned indexInTable;   // its OffsetLocation::m_indexInTable
};
struct StringSwitchRanks {
    // Indexed by string table; for a table emission gives an inline tree (1 to maximumInlineStringSwitchCaseCount
    // keys, JIT::emit_op_switch_string), every key sorted by signed address, so element r is rank r (section 3.7);
    // empty for any other table.
    Vector<Vector<StringSwitchRank>> tables;
};
StringSwitchRanks rankStringSwitches(const UnlinkedCodeBlock&);

struct ResolutionContext {
    VM& vm;
    UnlinkedCodeBlock* ucb { nullptr }; // UCB targets; null when resolving support or VM data only. Not const: the
                                        // arithmetic profile accessors the UCB targets read have no const overload.
    // Artifact targets; null or empty when resolving support or data only.
    const void* imageStart { nullptr };
    std::span<void* const> mathICs;
    std::span<const void* const> switchTableBases;
    std::span<const StringJumpTable> stringSwitchTables; // with their code locations filled (section 3.7)
    std::span<const void* const> snippetStarts;
    const StringSwitchRanks* ranks { nullptr };
};
const void* resolveTarget(const ResolutionContext&, const ImageTarget&); // a valid target only
CodePtr<NoPtrTag> resolveSupport(VM&, const ImageTarget&);               // support kinds only
}
```

`resolveTarget` takes a valid target: the producer's recording makes every target valid (I3), and strict checks it (V3, V5, V6, U3, U6 and U7) before anything resolves, so normal mode resolves without checking (THREAD Session); debug builds `ASSERT` the conditions of table 3.3. `SwitchStringRankCase` reads `stringSwitchTables`, which the producer's `JIT::link` and the consumer's step 7 of section 10.3 have filled. Resolving a support key may generate support code through the native lookup, as the VM's first use of it would (THREAD Restoration): `VM::getCTIStub` under `JITThunks::m_lock`, with must-succeed executable allocation, on the VM thread.

### 3.9 Recording rules

Recording follows these rules, which keep every byte outside a fixup the same in every process for the same inputs and draws (I1).

- R1. A reference is emitted through a helper of section 4.3, which takes its typed target. Without a recorder the helper emits exactly the native instruction sequence (I4).
- R2. A recorded `Pointer` uses `moveWithPatch(TrustedImmPtr(value), reg)` and records the `DataLabelPtr`. Nothing is folded into an immediate operand, a displacement or arithmetic, and nothing is blinded.
- R3. An absolute-address operation (load, store, compare, test, read-modify-write) materializes the address as a `Pointer` into a temp and then operates on `Address(temp)`, in place of both x86_64 absolute-address shapes (N1). The temp is `scratchRegister()` (r11) on x86_64 and the memory temp (`x17`, through `MacroAssemblerARM64::memoryTempRegisterForReference`, section 14.1) on ARM64; every operation the helpers use with that base leaves the temp alone (section 4.3 lists them).
- R4. On ARM64, materializing a reference into a cached temp register invalidates that register's cache, so no later move derives a value from a reference's bits. Recording never reuses a cached temp, which meets THREAD Capture's reuse rule. History: [Cached temp registers](SPEC-image-history.md#cached-temp-registers).
- R5. `structureIDBase` has one form on each architecture: a `Pointer` into `scratchRegister()` (r11 on x86_64, the data temp `x16` on ARM64) followed by a register `or64`.
- R6. On ARM64, a conditional branch to support code or to another allocation is linked inside its own allocation to a veneer, a `b` emitted after the rest of the code, and only that `b` carries a fixup (section 4.5). On x86_64 a conditional external branch is a `jcc rel32` and carries a `Jump` fixup directly. Once a recorder stops recording, an ARM64 conditional external branch is linked natively, with no veneer (section 4.5, I19), and that recorder's record is never captured (section 4.8).
- R7. Support code is reached through `Call` and `Jump` fixups whose targets are support keys (section 3.4); the far call to a C++ operation is a `Pointer` fixup at the call's pointer placeholder (N2).
- R8. A code address the image's side tables hold (call and IC return points, switch and code map entries, MathIC locations) is an offset into the image or snippet, never a fixup. A fixup holds every reference in the bytes and nothing else.
- R9. Labels are recorded at emission as pre-compaction `AssemblerLabel`s and translated after `LinkBuffer` copied and compacted the code (section 4.7).

## 4. Recording

### 4.1 Who records

A baseline compilation records when `JIT::compileAndLinkWithoutFinalizing` starts, `producerContext(vm)` (R-INT-1) returns a context and its plan's `jitCacheRecordsImage()` is true, which `JIT::compileAndLinkWithoutFinalizing` reads through `JIT::m_plan` (R-INT-12); in twins builds it also records when `JIT::setJITCacheTwin` already gave the JIT a twin recorder (section 11.3, step 4). The flag says whether the CB's UCB has a record, which a UCB gets only at a request point, before it is published (SPEC-ucb.md section 6.2).

A MathIC regeneration records when the CB `generateOutOfLine` receives (section 6.2) runs `BaselineJIT`, `producerContext` of that CB's VM returns a context, and the CB's `BaselineJITCode` carries an image record in state `Complete` (section 5). In a ConsumerProducer VM this includes imported images, whose record is rebuilt at import (section 10.3, step 14). A twin regeneration, which exists only in twins builds, always records, into its twin compile's record (section 11.3, step 5).

Since a context exists only while a producing VM's cache activity is on (R-INT-1), every recording compilation runs with the fixed options of options.md at their required values, which `start` checks, and without the per-bytecode profiler, which only `useProfiler` creates (N29). None meets Debugger-mode bytecode (`op_debug`, the ShadowChicken opcodes) or builds a PC-to-origin map: both need an attached debugger or options fixed off, and a debugger attach turns cache activity off for good (THREAD Session; N15, N29). A compilation that read the context just before it turned null records an image that is never captured: the context turns null only when production ends, for good (R-INT-1), and capture runs only while production is active (R-INT-6). Other VMs emit unchanged.

A compilation's record becomes `Unrecordable` with a reason, and is never captured, when emission meets any of the following. None of them is a fault.

| reason | where |
|---|---|
| `NotShareable` | `JIT::link` finds `m_isShareable` false, which only the profiler opcodes cause (N29, census C24) |
| `SuperSamplerOpcode` | `JIT::emit_op_super_sampler_begin`, `JIT::emit_op_super_sampler_end`, whose opcodes only builtin-mode parsing produces while `exposePrivateIdentifiers` stays fixed off (options.md; census C20) |
| `UnannotatedSupportLink` | the guard of section 4.4 |
| `UnannotatedPointerArgument` | the guard of section 4.4 |
| `UnannotatedReference` | a path that baseline code never takes and that would embed a reference the census does not cover (the census names each such path) |
| `ForeignCodeSymbol` | a C++ function outside the engine's text segment (section 3.5) |
| `UnknownVMAddress` | a helper given a VM address outside table 3.6 |
| `InconsistentRecord` | `finishBaselineCompile` or a regeneration hook finds two footprints that overlap, one past its code's linked size, a MathIC with only some of its locations set, or a MathIC with inline code and no `Operation` fixup at its slow call; emission meets an arithmetic profile in neither UCB vector (census A14); a regeneration meets a MathIC its `Complete` record does not list (section 6.2). All are programming errors, which debug builds `ASSERT` (sections 4.7, 6.1 and 6.2; census A14) |
| `UnknownIdentifier` | capture finds a mold identifier that is neither a UCB identifier nor an immortal name (section 9) |
| `MathICProvenance` | capture finds a MathIC snippet without matching provenance (section 9) |

History: [When a compilation records](SPEC-image-history.md#when-a-compilation-records).

### 4.2 The recorder

```cpp
namespace JSC::JITCache {

enum class RecordingScope : uint8_t { BaselineCompile, MathICSnippet };

class ImageRecorder {
    WTF_MAKE_NONCOPYABLE(ImageRecorder);
    WTF_MAKE_TZONE_ALLOCATED(ImageRecorder);
public:
    ImageRecorder(RecordingScope, VM&, UnlinkedCodeBlock&, Ref<ProducerBudget>&&);
#if ENABLE(JITCACHE_TWINS)
    // A twin compile (section 11.3, step 4): the producer's seeds, and the producer's compile inputs for the answers emission asks for.
    ImageRecorder(RecordingScope, VM&, UnlinkedCodeBlock&, Ref<ProducerBudget>&&, const TwinSeeds&, const TwinCompileInputs&);
#endif
    ~ImageRecorder(); // releases every byte it charged and did not hand to a record

    void attachTo(AbstractMacroAssemblerBase&); // setJITCacheRecorder(this); a twin recorder also seeds the assembler
    bool isRecording() const;                   // false once Unrecordable or Incomplete (section 4.8)

    // Emission. recordPointer, recordCall, recordJump and deferConditionalJump are called only by the helpers of
    // section 4.3; the rest by those helpers or by the call sites that sections 4.5, 6.1 and 7 and the census name.
    void recordPointer(AssemblerLabel site, const ImageTarget&);
    void recordCall(AssemblerLabel site, const ImageTarget&);
    void recordJump(AssemblerLabel site, const ImageTarget&);
    void deferConditionalJump(MacroAssembler&, MacroAssembler::Jump, const ImageTarget&, CodeLocationLabel<NoPtrTag> nativeTarget); // ARM64 only
    void emitVeneers(MacroAssembler&);                                   // before LinkBuffer
    unsigned noteMathIC(MathICKind, const void* mathIC, BytecodeIndex);  // BaselineCompile only
    ImageTarget mathICTarget(const void* mathIC) const;                  // MathIC(the index noteMathIC gave mathIC); RELEASE_ASSERTs it was noted
    std::optional<ImageTarget> arithProfileTarget(const void* profile) const; // index by address range in the UCB; nullopt outside both vectors (census A14)
    std::optional<ImageTarget> vmAddressTarget(const void* address) const; // table 3.6 by address; nullopt outside it
    void markUnrecordable(Unrecordable);
    BakedFactsBuilder& bakedFacts();                                     // BaselineCompile only

    bool isLinkingSupport() const { return m_linkingSupport; }
    class SupportLinkScope;                                              // sets m_linkingSupport while a helper links

#if ENABLE(JITCACHE_TWINS)
    void didInitializeRandom(uint32_t seed);         // from AbstractMacroAssemblerBase::initializeRandom: records the assembler's seed
    uint32_t binarySwitchSeed(uint32_t drawn);       // records and returns drawn; a twin recorder returns the next recorded seed instead
    void recordCompileInputs(TwinCompileInputs&&);   // BaselineCompile only, at compile start (section 11.1)
    // BaselineCompile only, from the strict-equality templates (N25): records chosen as the instruction's compile input and
    // returns it; a twin recorder returns the producer's input for the instruction instead (section 11.1).
    StrictEqualityAtomOperand strictEqualityAtomOperand(BytecodeIndex, StrictEqualityAtomOperand chosen);
#endif

    // Finishing.
    std::unique_ptr<ImageRecord> finishBaselineCompile(LinkBuffer&, BaselineJITCode&, std::span<const FarCallRecord> farCalls);
    std::optional<SnippetProvenance> finishSnippet(LinkBuffer&);
};

} // namespace JSC::JITCache
```

The recorder lives in the `JIT` object (`JIT::m_imageRecorder`) or on the stack of `JITMathIC::generateOutOfLine`, and `attachTo` attaches it to the assembler through `AbstractMacroAssemblerBase::setJITCacheRecorder` before anything is emitted. Only the thread running that compilation or regeneration uses it, so it needs no lock. It reads the UCB and the VM (addresses only); it never allocates a cell, takes a JSC lock, or reads a CB. In twins builds the JIT reads the CB's compile inputs itself and hands the snapshot to `recordCompileInputs`, and the strict-equality templates hand their atom choice to `strictEqualityAtomOperand` (section 11.1). Code in an exported header reaches the recorder only through the hooks of section 4.3 (section 14.4).

### 4.3 Emission helpers

`ImageEmission.h` declares the functions emitters use for references; inline functions of exported headers reach the same helpers through the hooks at the end of this section. Each helper takes a `MacroAssembler&`, finds the recorder through `jitCacheRecorder()`, and without one emits the native sequence given in the second column. The exception is `StringSwitchRecording`, which exists only under a recorder and takes it at construction.

```cpp
namespace JSC::JITCache {

class ImageReference final : public CCallHelpers::ConstantMaterializer {
public:
    ImageReference(const ImageTarget&, const void* value);
    static ImageReference vmAddress(VM&, VMAddress);
    static ImageReference vmCell(VM&, VMCell);
    static ImageReference processThunk(ProcessThunk);              // DefaultCall only
    static ImageReference ucbConstantCell(VM&, UnlinkedCodeBlock&, VirtualRegister);
    static ImageReference ucbConstantAtom(VM&, UnlinkedCodeBlock&, VirtualRegister);
    static ImageReference ucbIdentifier(VM&, UnlinkedCodeBlock&, unsigned identifierIndex);
    static ImageReference arithProfile(VM&, UnlinkedCodeBlock&, const BinaryArithProfile&);
    static ImageReference arithProfile(VM&, UnlinkedCodeBlock&, const UnaryArithProfile&);
    static ImageReference mathIC(const void* mathIC);              // target from ImageRecorder::mathICTarget when recorded
    static ImageReference switchTableBase(unsigned tableIndex, const void* base);

    void materialize(CCallHelpers&, GPRReg) const;                 // for setupArguments
    void store(CCallHelpers&, CCallHelpers::Address) const;
    const void* value() const;
    const ImageTarget& target() const;
};

enum class StoreValueKind : uint8_t { Value, Trusted }; // AssemblyHelpers::storeValue or storeTrustedValue

void moveReference(MacroAssembler&, const ImageReference&, GPRReg);
MacroAssembler::Jump branchPtrWithReference(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg left, const ImageReference& right);
void storeReferenceValue(AssemblyHelpers&, const ImageReference& cell, MacroAssembler::Address, StoreValueKind);
void moveReferenceValue(AssemblyHelpers&, const ImageReference& cell, GPRReg);
MacroAssembler::Jump branchPtrAtReference(MacroAssembler&, MacroAssembler::RelationalCondition, const ImageReference& address, GPRReg right);
MacroAssembler::Jump branchTestPtrAtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address);
MacroAssembler::Jump branchTest32AtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address, MacroAssembler::TrustedImm32 mask);
MacroAssembler::Jump branchTest8AtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address);
MacroAssembler::Jump branch32WithReferenceAt(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg left, const ImageReference& rightAddress);
void store8AtReference(MacroAssembler&, MacroAssembler::TrustedImm32, const ImageReference& address);
void storePtrAtReference(MacroAssembler&, GPRReg, const ImageReference& address);
void loadPtrAtReference(MacroAssembler&, const ImageReference& address, GPRReg);
void or16AtReference(MacroAssembler&, MacroAssembler::TrustedImm32, const ImageReference& address);
void or16AtReference(MacroAssembler&, GPRReg, const ImageReference& address);
void orStructureIDBase(MacroAssembler&, GPRReg source, GPRReg dest);

void nearCallSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void nearTailCallSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void jumpSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void linkJumpToSupport(MacroAssembler&, VM&, MacroAssembler::Jump, const ImageTarget& supportKey);

void jumpToImage(MacroAssembler&, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag>);
void linkJumpsToImage(MacroAssembler&, const MacroAssembler::JumpList&, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag>);

// The ranked comparisons of an inline switch_string (section 3.7), which JIT::emit_op_switch_string creates only when
// it has a recorder and hands to its BinarySwitch through setRankedComparisons (section 14.1). A rank is the case's
// position among BinarySwitch's cases sorted by value: advance passes it to branch, and caseRank() returns it.
class StringSwitchRecording final : public BinarySwitchRankedComparisons {
public:
    StringSwitchRecording(ImageRecorder&, unsigned tableIndex);
    MacroAssembler::Jump branch(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg value, unsigned rank, intptr_t key) final;
    void recordCase(MacroAssembler::Jump leaf, unsigned rank); // the leaf jump() that executes rank, before addJump
};

} // namespace JSC::JITCache
```

| helper | native sequence without a recorder | recorded sequence |
|---|---|---|
| `ImageReference::materialize` | `move(TrustedImmPtr(value), gpr)` | `Pointer` into `gpr` |
| `ImageReference::store` | `storePtr(TrustedImmPtr(value), address)` | `Pointer` into the temp of R3, `storePtr(temp, address)` |
| `moveReference` | `move(TrustedImmPtr(value), gpr)` | `Pointer` into `gpr` |
| `branchPtrWithReference` | `branchPtr(cond, left, TrustedImmPtr(value))` | `Pointer` into `scratchRegister()`, `branchPtr(cond, left, scratch)` |
| `storeReferenceValue` | `storeValue(JSValue(cell), address)` or `storeTrustedValue` per `StoreValueKind` | `Pointer` into `scratchRegister()`, `store64(scratch, address)` |
| `moveReferenceValue` | `moveValue(JSValue(cell), gpr)` | `Pointer` into `gpr` |
| `branchPtrAtReference` | `branchPtr(cond, AbsoluteAddress(value), right)` | `Pointer` into the R3 temp, `branchPtr(cond, Address(temp), right)` |
| `branchTestPtrAtReference` | `branchTestPtr(cond, AbsoluteAddress(value))` | `branchTestPtr(cond, Address(temp))` |
| `branchTest32AtReference` | `branchTest32(cond, AbsoluteAddress(value), mask)` | `branchTest32(cond, Address(temp), mask)` |
| `branchTest8AtReference` | `branchTest8(cond, AbsoluteAddress(value))` | `branchTest8(cond, Address(temp))` |
| `branch32WithReferenceAt` | `branch32(cond, left, AbsoluteAddress(value))` | `branch32(cond, left, Address(temp))` |
| `store8AtReference` | `store8(imm, value)` | `store8(imm, Address(temp))` |
| `storePtrAtReference` | `storePtr(gpr, value)` | `storePtr(gpr, Address(temp))` |
| `loadPtrAtReference` | `loadPtr(value, gpr)` | `loadPtr(Address(temp), gpr)` |
| `or16AtReference` | `or16(mask, AbsoluteAddress(value))` | `or16(mask, Address(temp))` |
| `orStructureIDBase` | `or64(TrustedImm64(structureIDBase()), source, dest)` | R5 |
| `nearCallSupport` | `nearCallThunk(resolveSupport(vm, key))` | the same, recording a `Call` at `Call::m_label` |
| `nearTailCallSupport` | `nearTailCallThunk(resolveSupport(vm, key))` | the same, recording a `Jump` at the call's label |
| `jumpSupport` | `jumpThunk(resolveSupport(vm, key))` | the same, recording a `Jump` at the jump's label |
| `linkJumpToSupport` | `jump.linkThunk(resolveSupport(vm, key), &masm)` | x86_64 or unconditional: the same, recording a `Jump`; ARM64 conditional: `deferConditionalJump` with the resolved location (section 4.5) |
| `jumpToImage`, `linkJumpsToImage` | `jumpThunk(location)`, `jumps.linkThunk(location, &masm)` | as `jumpSupport` and `linkJumpToSupport` with target `ImageOffset(imageOffset)` |
| `StringSwitchRecording::branch` | none: the JIT passes no recording, and `BinarySwitch` emits `branchPtr(cond, value, ImmPtr(key))` itself | `branchPtrWithReference` with target `SwitchStringRankAtom(table, rank)` |
| `StringSwitchRecording::recordCase` | none: the JIT emits the leaf `jump()` either way | records a `Jump` fixup at the leaf jump's label, target `SwitchStringRankCase(table, rank)` |

A helper's native sequence is the one its sites already emit. `moveValue` and `storeValue` emit an untrusted `Imm64`, which on x86_64 draws from the assembler's random source and may blind the constant; a trusted move draws nothing, so putting one in place of the other changes the site's bytes whenever blinding would fire and shifts every later blinding decision of the compilation. A site whose native code uses `moveValue` or `storeValue` therefore goes through `moveReferenceValue` or `storeReferenceValue`, and `moveReference` serves only native trusted moves.

The temp of each recorded sequence is read before anything else writes a temp:

- On x86_64, every operation above on `Address(r11)` encodes `r11` as its base and uses no scratch register (`orw_im`, `orw_rm`, `movb`, `cmp`, `test`).
- On ARM64, the compare, test and load forms on `Address(x17)` load into `x16` or into `x17` itself, which consumes the address in the same instruction; `store8` and `storePtr` write through `x17` after placing the value in `x16` or a GPR. `or16AtReference` is emitted explicitly as `load16` from `Address(x17)` into `x16`, the `or` on `x16`, and `store16` back. Its immediate form accepts only a mask that needs no temp, a valid 32-bit logical immediate (`LogicalImmediate::create32`). Every baseline site meets this (the masks are single `ObservedResults` bits, `BinaryArithProfile::specialFastPathBit` and the contiguous `UnaryArithProfile::observedNumberBits()`, and `ArithProfile::emitSetDouble` passes its four-bit mask in a register on ARM64), and the helper `RELEASE_ASSERT`s it under a recorder.
- `branchPtrWithReference`, `storeReferenceValue` and `orStructureIDBase` hold the value in `scratchRegister()` (`r11`, or `x16` on ARM64), and the register-register operation that follows uses no temp.

A helper `ASSERT`s that its register operands differ from its temps.

The support helpers and the value factories of `ImageReference` that take a `VM&` compute their value with `resolveSupport` or `resolveTarget` (section 3.8) for the producing VM and UCB, so the emitted value and the consumer's resolution come from one function. `processThunk` reads the process-wide entry its kind names, which `resolveTarget` gives in every VM; `mathIC` and `switchTableBase` take the producer's own artifact value. A support helper or factory takes the VM every census site already holds: `JIT::vm()`, the `VM&` parameter of `AssemblyHelpers::emitVirtualCallWithoutMovingGlobalObject`, or `m_jit->vm()` in `JITSlowPathCall::call`. A `MacroAssembler` holds none, and `AssemblyHelpers::vm()` would dereference the null `m_codeBlock` the `JIT` passes up (`JSInterfaceJIT(&vm, nullptr)`).

The census also reaches inline functions of headers JSC exports, which cannot include `ImageEmission.h` (section 14.4). They reach recording through hooks that the exported header declares itself, with only JSC types and forward-declared `JITCache` classes, and that `ImageEmission.cpp` defines:

```cpp
// assembler/AbstractMacroAssembler.h
namespace JSC::JITCache {
class ImageRecorder;
void noteSupportLink(ImageRecorder&);                       // section 4.4
}

// jit/AssemblyHelpers.h
namespace JSC::JITCache {
void storePtrToVMAddress(MacroAssembler&, GPRReg, const void* address);
void loadPtrFromVMAddress(MacroAssembler&, const void* address, GPRReg);
MacroAssembler::Jump branch32WithVMAddress(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg left, const void* address);
MacroAssembler::Jump branchTest8AtVMAddress(MacroAssembler&, MacroAssembler::ResultCondition, const void* address);
void noteUnannotatedReference(ImageRecorder&);              // markUnrecordable(UnannotatedReference)
}

// jit/CCallHelpers.h
namespace JSC::JITCache {
void notePointerArgument(ImageRecorder&);                   // section 4.4
}
```

An inline function calls a hook only after it found a recorder on its assembler (`if (auto* recorder = jitCacheRecorder()) [[unlikely]]`), so emission without a recorder stays inline and byte for byte native (I4). Each VM-address hook classifies its address with `ImageRecorder::vmAddressTarget` and emits the recorded sequence of the helper with the same operation (`storePtrAtReference`, `loadPtrAtReference`, `branch32WithReferenceAt`, `branchTest8AtReference`); for an address outside table 3.6 it marks the record `Unrecordable(UnknownVMAddress)` and emits the native sequence. The census preamble lists the rows whose sites reach recording through these hooks. History: [Finding references](SPEC-image-history.md#finding-references).

### 4.4 Guards

Two guards turn a missed site into an unrecorded compilation instead of a wrong image:

- `Call::linkThunk` and `Jump::linkThunk` in `AbstractMacroAssembler.h` call `noteSupportLink(*recorder)` when the assembler has a recorder, and `noteSupportLink` marks the record `UnannotatedSupportLink` unless `isLinkingSupport()` holds. The helpers of section 4.3 set it through `SupportLinkScope` around their own `linkThunk` calls. `nearCallThunk`, `nearTailCallThunk` and `jumpThunk` reach these two functions, and `PatchableJump::linkThunk` and `JumpList::linkThunk` delegate to `Jump::linkThunk`, so every thunk link is covered.
- `CCallHelpers::setupArgumentsImpl`, in its `TrustedImm` overload, calls `notePointerArgument(*recorder)` when the assembler has a recorder and the argument is a `TrustedImmPtr` with a nonzero value, and `notePointerArgument` marks the record `UnannotatedPointerArgument`. Every nonzero pointer argument of baseline code is a reference (section 4.6), so it must arrive as an `ImageReference`.

A pointer immediate moved into a register or compared outside these paths is not guarded. The twin comparison detects it only where the producer's and the consumer's values for it differ: the restored image keeps the producer's value and the twin takes the consumer's (section 11.4, the relocation clause). History: [Finding references](SPEC-image-history.md#finding-references).

### 4.5 Veneers

On ARM64, `linkJumpToSupport` and `linkJumpsToImage` hand each conditional jump to `deferConditionalJump`, with the location the native link would use (`resolveSupport(vm, key)`, or the image location `linkJumpsToImage` received). `deferConditionalJump` appends the jump to its target's group. Groups keep the order in which their targets first appear, and a linear search finds a target's group, since a compilation has few distinct external targets. Each group holds its target, the native location and its jumps, and its storage is charged as it grows (section 4.8). When the recorder is no longer recording, or the charge for the group's storage is refused, `deferConditionalJump` instead links the jump at once with `jump.linkThunk(nativeTarget, &masm)` inside a `SupportLinkScope`: the native link, with no veneer.

Before the `LinkBuffer` is built, `emitVeneers` walks the groups and, for each, emits `Label veneer = label()`, links every jump of the group to it with `Jump::linkTo` (an internal branch, which compaction may shorten), emits `Jump b = jump()`, links `b` to the group's native location with `linkThunk` inside a `SupportLinkScope`, and records a `Jump` fixup at `b`. It allocates nothing beyond that fixup record. It emits and links the veneer of every deferred jump even when the recorder stopped recording after deferring it, and records the veneer's fixup only while the recorder still records. The calls sit in `JIT::compileAndLinkWithoutFinalizing` after the jump to `ThrowStackOverflowAtPrologue` and in `JITMathIC::generateOutOfLine` before each snippet `LinkBuffer`. On x86_64, `emitVeneers` does nothing. History: [ARM64 veneers](SPEC-image-history.md#arm64-veneers).

### 4.6 The census

[SPEC-image.sites.md](SPEC-image.sites.md) lists every site, in the functions this lane edits, where baseline image or MathIC snippet code embeds a reference, with its target kind and helper, under these rules:

- C1. Every reference a function emits under a recorder goes through a section 4.3 helper or through `JIT::appendCall` (far calls, recorded centrally in `JIT::link`).
- C2. A shared helper (`AssemblyHelpers`, `CCallHelpers`, `ArithProfile`, `CallLinkInfo`, `BinarySwitch`, `JITSlowPathCall`) is edited once, at the helper, and its DFG, FTL and thunk callers see no change, since their assemblers carry no recorder.
- C3. A site whose reference exists only under a fixed option of options.md (exception fuzz, execution tracing, the per-bytecode profiler, loop-hint counters) or only in Debugger-mode bytecode (`op_debug`, the ShadowChicken opcodes) is not annotated: no recording compilation meets either (section 4.1).
- C4. A site in a thunk generator is support code, never image code, and is not annotated.
- C5. Literals the census names (for example `JSCell::seenMultipleCalleeObjects()`, encoded JSValues of `undefined`, `null` and booleans) stay native.

### 4.7 Finishing a compilation

`JIT::link` calls `m_imageRecorder->finishBaselineCompile(patchBuffer, jitCode, m_farCalls.span())` after building the `BaselineJITCode` and before returning it. It:

1. Records each `JIT::m_farCalls` entry with a nonnull callee as a `Pointer` fixup at `farCallPointerSite(offsetOf(call label))`, target `Operation(CodeSymbol::of(callee))`. `JIT::link` passes `m_farCalls.span()`, since the member is private and `JIT` gains no friend. `farCallPointerSite` subtracts `REPATCH_OFFSET_CALL_R11` on x86_64 and on ARM64 adds `-(ARM64Assembler::NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS + 1) * 4`, the value of the protected `MacroAssemblerARM64::REPATCH_OFFSET_CALL_TO_POINTER` computed from a public constant (`MacroAssemblerARM64::call(PtrTag)` asserts that offset).
2. Takes `patchBuffer.size()` as the record's `codeSize` (section 3.1, N20), translates every recorded `AssemblerLabel` with `LinkBuffer::offsetOf(AssemblerLabel)` (section 14.1), puts the fixups in footprint order (section 3.2) and checks that each footprint starts at or after the end of the one before it and lies inside `[0, codeSize)`. A violation is a programming error: an `ASSERT` in debug builds, `Unrecordable(InconsistentRecord)` in release builds. History: [The linked size](SPEC-image-history.md#the-linked-size).
3. Builds the MathIC entries from `noteMathIC` and each IC's locations after the link tasks: an IC with inline code gets its slow call's `Operation` fixup, an IC without inline code none (section 6.1).
4. Marks the record `Unrecordable(NotShareable)` when `m_isShareable` is false.
5. Moves the baked facts in and attaches the record to `jitCode.m_jitCacheImageRecord`.
6. Shrinks the record's vectors to their sizes and releases every charged byte the record does not hold, as section 4.8 requires of a finished compilation.

A recorder that stopped recording skips steps 1 to 3 and attaches a record holding only its state and reason, which step 6 leaves charged for the record object alone. The record object is charged with the recorder's first step, so a recorder whose first charge was refused attaches no record, which capture and regeneration treat as an image without a record.

`JIT::link` returning null (no executable memory) drops the recorder, which releases its charges.

### 4.8 Charging

Every byte the recorder or a record allocates is charged to the producer budget before the allocation, with `ProducerBudget::tryCharge` (R-INT-2), from whatever thread records. Charges come in steps of `kRecordChargeStep` bytes (4 KiB): before any of the recorder's containers would hold more than the bytes already charged, the recorder charges the next step, which keeps charges off the per-fixup path. A finished compilation or regeneration gives back what its steps charged beyond the bytes the record then holds (section 4.7, step 6, and `didLinkSnippet`), so a live record's charge follows its bytes, not the steps it took. Destroying a recorder or record releases exactly what it charged (I14).

A refused charge makes the recorder `Incomplete` for the rest of that compilation or regeneration, and the record it produces `Incomplete`. A recorder that became `Unrecordable` behaves the same way. From then on:

- every recording function (`recordPointer`, `recordCall`, `recordJump`, the baked-facts builder and, in twins builds, the seed hooks, `recordCompileInputs` and `strictEqualityAtomOperand`) returns without storing anything; `noteMathIC` only returns the next index, and `binarySwitchSeed` and `strictEqualityAtomOperand` return the value they were given, except in a twin recorder, which keeps answering from the producer's seeds and inputs so that the twin's emission stays the producer's;
- the helpers of section 4.3 keep emitting their recorded forms, which are correct code; the only form that needs storage, the ARM64 deferral of a conditional external jump, becomes the native link (section 4.5);
- `emitVeneers` still emits and links the veneers of the jumps deferred before the refusal, whose storage was charged when they were deferred.

So every branch the compilation or regeneration emits is linked whatever the budget answers (I19), and the record, being `Incomplete`, is never captured. The budget's refusal of later charges and its recording fault are R-INT-2's; this lane raises nothing itself. History: [Charging](SPEC-image-history.md#charging).

## 5. The image record

```cpp
namespace JSC::JITCache {

enum class RecordState : uint8_t { Complete, Unrecordable, Incomplete };

struct ImageFixup {
    uint32_t site;
    FixupForm form;
    ImageTarget target;
};

struct SnippetProvenance {
    const void* start;                 // the snippet allocation this provenance describes
    uint32_t size;                     // the snippet LinkBuffer's size() (section 3.1)
    Vector<ImageFixup> fixups;         // sites relative to the snippet start
};

enum class MathICKind : uint8_t { Add = 1, Sub = 2, Mul = 3, Negate = 4 }; // the kind byte of section 8.2, region 9

struct MathICRecord {
    MathICKind kind;
    BytecodeIndex bytecodeIndex;
    void* mathIC;                                // JITAddIC*, JITSubIC*, JITMulIC* or JITNegIC*, owned by the BaselineJITCode
    std::optional<uint32_t> slowCallPointerSite; // site of the slow call's Operation fixup; empty when the IC has no inline code (section 6.1)
    std::optional<SnippetProvenance> snippet;    // only for an IC with inline code
};

class ImageRecord {
    WTF_MAKE_NONCOPYABLE(ImageRecord);
    WTF_MAKE_TZONE_ALLOCATED(ImageRecord);
public:
    RecordState state() const;
    Unrecordable unrecordableReason() const;
    uint32_t codeSize() const;                         // the linked size (section 3.1)
    const Vector<ImageFixup>& fixups() const;          // in footprint order (section 3.2)
    const Vector<MathICRecord>& mathICs() const;       // in emission order; index = MathIC index
    const BakedFacts& bakedFacts() const;

    // VM thread, regeneration hooks of section 6.2.
    std::optional<unsigned> mathICIndex(const void* mathIC) const;
    void didReplaceSlowCall(unsigned mathICIndex, CodePtr<CFunctionPtrTag>);
    void didGenerateSnippet(unsigned mathICIndex, SnippetProvenance&&);
    void didRewriteInlineStart(unsigned mathICIndex);  // section 6.2, edit 1
    void markUnrecordable(Unrecordable);
    void markIncomplete();

#if ENABLE(JITCACHE_TWINS)
    TwinData& twinData();                              // seeds, compile inputs, regeneration log (section 11.1)
#endif

    ~ImageRecord(); // releases its charge, on any thread
private:
    Ref<ProducerBudget> m_budget;
    size_t m_chargedBytes;
    ...
};

} // namespace JSC::JITCache
```

`BaselineJITCode` gains `std::unique_ptr<JITCache::ImageRecord> m_jitCacheImageRecord`. It is null in VMs that do not record, and in imported images everywhere except in a ConsumerProducer VM; the twin compile of a twins build records in any VM, and its code dies with the check (section 11.3).

State. `Complete` means every reference in the image and in the current snippet of every MathIC has exactly one fixup. `Unrecordable` and `Incomplete` are final; a record in either state is never captured, records nothing more and, when the recorder stopped before finishing, holds only its state and reason (section 4.7). `didRewriteInlineStart`, `didGenerateSnippet` and `didReplaceSlowCall` keep a `Complete` record complete or, on a refused charge, make it `Incomplete`. In twins builds the record also keeps the twin data of section 11.1, charged like the rest of the record.

Threads. The recorder builds the record on the compiling thread. After `JIT::link` returns, the record is read and written only on the thread that holds the VM's API lock (I6): MathIC regenerations run in JIT operations on that thread, and capture runs there with JS paused. Destruction can happen on any thread, because the last `BaselineJITCode` reference can drop in a sweep, in `Heap::releaseUnusedSharedBaselineCode` or in a cancelled plan; the destructor only releases its charge, which `ProducerBudget::release` allows from any thread (R-INT-2). The record holds no cell, so GC never visits it.

Lifetime. The record lives exactly as long as its `BaselineJITCode`, as THREAD Capture requires, and `MathICRecord::mathIC` points into that `BaselineJITCode`'s `MathICHolder`, which has the same lifetime.

## 6. MathICs

### 6.1 Records

`JIT::emit_op_add`, `emit_op_sub`, `emit_op_mul` and `emit_op_negate` call `noteMathIC(kind, ic, m_bytecodeIndex)` right after `MathICHolder::addJIT*IC`; the call returns the MathIC index, the IC's position in emission order. Every IC the holder owns is noted, so the record and the holder count the same ICs (S3).

A MathIC has inline code or none (N17). `finishBaselineCompile` runs after the link tasks, which `FINALIZE_BASELINE_CODE` runs inside `JIT::link`, and reads each noted IC's locations:

- all four null: the entry has no inline code, and `slowCallPointerSite` is empty;
- all four set: the entry has inline code, and `slowCallPointerSite = farCallPointerSite(offset of m_slowPathCallLocation)`, where step 1 of section 4.7 put the slow call's `Operation` fixup (I7);
- any other combination, or no `Operation` fixup at that site, is a programming error: an `ASSERT` in debug builds and `Unrecordable(InconsistentRecord)` in release builds.

The locations, `m_generateFastPathOnRepatch` and `m_code` are read from the IC at capture, not copied into the record. History: [MathICs without inline code](SPEC-image-history.md#mathics-without-inline-code).

### 6.2 Regeneration

`JITMathIC::generateOutOfLine` takes its regeneration object explicitly. The existing signature becomes a wrapper, so every native caller keeps it unchanged, and only the twin replay (section 11.3, step 5) passes its own object.

`JITMathIC.h` is an exported header (section 14.4) and defines the native body inside its class template, so the header keeps only the two declarations, with `JITCache::MathICRegeneration` forward-declared, and both definitions move to a new `jit/JITMathIC.cpp`. That file includes `ImageRecord.h` and `ImageEmission.h` and ends with explicit instantiation definitions of both members for the four specializations the header's typedefs name (`JITAddIC`, `JITMulIC`, `JITSubIC` and `JITNegIC`). Natively only the place where the function is compiled changes: its native callers, the repatching operations in `JITOperations.cpp`, call it through the declaration, and its only other caller is the twin replay. History: [Exported headers](SPEC-image-history.md#exported-headers).

```cpp
// jit/JITMathIC.h, in class template JITMathIC
void generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag> callReplacement);
void generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag> callReplacement, JITCache::MathICRegeneration&);

// jit/JITMathIC.cpp
template<typename GeneratorType, typename ArithProfileType>
void JITMathIC<GeneratorType, ArithProfileType>::generateOutOfLine(CodeBlock* codeBlock, CodePtr<CFunctionPtrTag> callReplacement)
{
    JITCache::MathICRegeneration regeneration(codeBlock, this, callReplacement, m_arithProfile ? m_arithProfile->bits() : 0);
    generateOutOfLine(codeBlock, callReplacement, regeneration);
}

// The native body, moved from the header, with the edits below.
template<typename GeneratorType, typename ArithProfileType>
void JITMathIC<GeneratorType, ArithProfileType>::generateOutOfLine(CodeBlock* codeBlock, CodePtr<CFunctionPtrTag> callReplacement, JITCache::MathICRegeneration& regeneration)
{
    ...
}

template void JITMathIC<JITAddGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>);
template void JITMathIC<JITAddGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>, JITCache::MathICRegeneration&);
// ... the same pair for <JITMulGenerator, BinaryArithProfile>, <JITSubGenerator, BinaryArithProfile> and <JITNegGenerator, UnaryArithProfile>
```

A native regeneration is active exactly when section 4.1 has it record. A `Complete` record lists every MathIC of its code, the record step 14 of section 10.3 rebuilds included, so a regeneration whose record does not list its IC is a programming error: an `ASSERT` in debug builds, and in release builds the regeneration marks the record `Unrecordable(InconsistentRecord)` and records nothing, so no capture reads a record that lacks the snippet. DFG and FTL MathICs, every non-recording VM, and a VM whose cache activity is off see an inactive object whose recording methods do nothing. A twin regeneration is always active (section 4.1) and draws its seeds from the replay (section 11.3, step 5).

```cpp
namespace JSC::JITCache {
class MathICRegeneration {
    WTF_MAKE_NONCOPYABLE(MathICRegeneration);
public:
    MathICRegeneration(CodeBlock*, const void* mathIC,
        CodePtr<CFunctionPtrTag> callReplacement, uint16_t profileBitsAtEntry);         // native; the last two feed only the twins builds' regeneration log (section 11.1)
#if ENABLE(JITCACHE_TWINS)
    MathICRegeneration(TwinReplay&, ImageRecord& twinRecord, const void* mathIC);      // twin replay
#endif
    ~MathICRegeneration(); // detaches any recorder it attached
    bool isActive() const;
    unsigned mathICIndex() const;                                     // 0 when inactive, which the helpers ignore without a recorder
    uint32_t imageOffset(CodeLocationLabel<JSInternalPtrTag>) const;  // likewise 0 when inactive
    void attach(CCallHelpers&);                               // a MathICSnippet recorder, through ImageRecorder::attachTo
    void emitVeneers(CCallHelpers&);
    void didLinkSnippet(LinkBuffer&, const MacroAssemblerCodeRef<JITStubRoutinePtrTag>&);
    void didRewriteInlineStart();                             // ImageRecord::didRewriteInlineStart on the active record
    void didReplaceSlowCall(CodePtr<CFunctionPtrTag>);
    void didFailToAllocate(VM&);
};
}
```

The edits to the body of `generateOutOfLine`, in its own order:

1. `linkJumpToOutOfLineSnippet` stays native and attaches no recorder, so no guard of section 4.4 fires: `jumpThunk(m_code.code())` emits one jump at offset 0 of the rewrite buffer, and that assembler never draws a random number (`AbstractMacroAssemblerBase::initializeRandom` runs only at the first `random()`). After `FINALIZE_CODE`, `regeneration.didRewriteInlineStart()` calls `ImageRecord::didRewriteInlineStart(i)`, which takes the rewritten bytes to be `[inlineStart, inlineStart + 4)` on ARM64 (one `b`) and `[inlineStart, inlineStart + 5)` on x86_64 (one `jmp rel32`). It drops every fixup whose footprint lies entirely inside them, which after a second rewrite is the earlier `SnippetEntry` fixup, and inserts a `Jump` fixup at the site V5 fixes (`inlineStart` on ARM64, `inlineStart + 5` on x86_64), target `SnippetEntry(i)`. A footprint the rewrite covers only in part would leave producer bits outside every fixup (I1, I8), so it is a programming error: an `ASSERT` in debug builds and `Unrecordable(InconsistentRecord)` in release builds. Fixups past the rewritten bytes stay, valid in the code the rewrite leaves dead. A `CannotCompile` CB has them: `JIT::emitMathICSlow` wires the repatching operation even after a full inline snippet, which `JIT::emitMathICFast` emits with its profile writes (A14) because `JITMathIC::generateInline`'s `shouldEmitProfiling` defaults to true. None straddles the rewritten bytes, since the `generateFastPath` of `JITAddGenerator`, `JITSubGenerator`, `JITMulGenerator` and `JITNegGenerator` each opens with a type check or a register move. History: [The inline-start rewrite](SPEC-image-history.md#the-inline-start-rewrite).
2. `replaceCall`: after `ftlThunkAwareRepatchCall`, `regeneration.didReplaceSlowCall(callReplacement)`, which retargets the fixup at `slowCallPointerSite` to `Operation(CodeSymbol::of(callReplacement))`, or makes the record `Unrecordable(ForeignCodeSymbol)` when the replacement lies outside the engine's text segment (section 3.5).
3. The fast-path snippet: after its `CCallHelpers`, `regeneration.attach(jit)`; `jit.jumpThunk(doneLocation())` becomes `jumpToImage(jit, regeneration.imageOffset(doneLocation()), doneLocation())`; `generationState.slowPathJumps.linkThunk(...)` becomes `linkJumpsToImage(...)` with the slow path start; `regeneration.emitVeneers(jit)` before the `LinkBuffer`; after `FINALIZE_CODE_FOR`, `regeneration.didLinkSnippet(linkBuffer, m_code)`.
4. The full snippet: the same, with `endJumpList.linkThunk` and `slowPathJumpList.linkThunk` replaced by `linkJumpsToImage`.
5. The native body tests `didFailToAllocate()` on two `LinkBuffer`s: the fast-path snippet's, whose failure falls through to `replaceCall` and the full snippet, and the full snippet's, whose failure returns; the inline rewrite's `LinkBuffer` writes into the image in place with `JITCompilationMustSucceed` and allocates nothing. Each failure branch first calls `regeneration.didFailToAllocate(codeBlock->vm())`, then continues natively. A native regeneration, active or not, calls `JITCache::didFailExecutableAllocation(vm, ExecutableAllocationSite::MathICSnippet)` there (R-INT-4), which covers MathIC regeneration in every tier and every configured VM (THREAD Execution); the fault is raised at the failure, inside the operation, before the native fallback writes anything. A twin regeneration marks its replay failed instead, so the twin check never faults the VM it runs in. History: [The MathIC allocation fault](SPEC-image-history.md#the-mathic-allocation-fault).

`didLinkSnippet` stores the snippet's provenance in the MathIC record, replacing any earlier one: the fixups from the snippet recorder in footprint order (section 3.2), the start of the allocation `m_code` now holds, and the linked size `linkBuffer.size()` (section 3.1); `m_code.size()` is the handle's size, which can be larger (N20). The earlier snippet's memory belonged to the `MacroAssemblerCodeRef` that `m_code` just replaced. All of this runs on the VM thread inside a JIT operation (for a twin regeneration, inside the twin check), holding the API lock and no `CodeBlock::m_lock`, allocates no cell, and charges through the record's budget (section 4.8). Once the new provenance is stored, `didLinkSnippet` releases the replaced provenance's bytes and the unused part of the regeneration's last step (section 4.8).

### 6.3 Generators

The consumer rebuilds each MathIC's generator from its instruction (THREAD Restoration). `JIT::emitMathICFast` stops computing operands and registers inline and calls a new static function that both it and the consumer use:

```cpp
// JIT.h, public
template<typename Op> static auto mathICGeneratorFor(const UnlinkedCodeBlock&, const JSInstruction*);
// OpAdd -> JITAddGenerator, OpSub -> JITSubGenerator, OpMul -> JITMulGenerator, OpNegate -> JITNegGenerator
```

It reproduces both overloads of `emitMathICFast` exactly, with `P` the profiled operation `emit_op_*` passes (a `static_assert` ties the two). Binary: `Generator(left, right, returnValueGPR, preferredArgumentGPR<P, 1>(), preferredArgumentGPR<P, 2>(), fpRegT0, fpRegT1, regT5)`, its `SnippetOperand`s from `m_operandTypes` and the UCB's int32 constants (the logic of `JIT::isOperandConstantInt` on the UCB). Unary (`negate`): `JITNegGenerator(src, src, preferredArgumentGPR<P, 0>())` with `src = preferredArgumentGPR<P, 1>()`, result and source in one register and the global object's register as scratch, and no operands. It reads only the UCB.

### 6.4 The `negate` operations

THREAD Caches has the regenerated `negate` IC's slow operation read its profile through the IC, as `add`, `sub` and `mul` do:

- New `operationArithNegateProfiledNoOptimize(JSGlobalObject*, EncodedJSValue, JITNegIC*)`, declared with `JSC_DECLARE_JIT_OPERATION` in `JITOperations.h`, reads `negIC->arithProfile()` and runs the same observation and computation as `operationArithNegateProfiled`; both call one file-static `profiledArithNegate(JSGlobalObject*, JSValue, UnaryArithProfile&)` so they cannot diverge.
- `operationArithNegateProfiledOptimize` passes `operationArithNegateProfiledNoOptimize` to `generateOutOfLine` instead of `operationArithNegateProfiled`.

`operationArithNegateProfiled` keeps its signature for the calls that pass a profile (`JIT::emitMathICFast` without inline code, `JIT::emitMathICSlow` without repatching). DFG and FTL use `operationArithNegateOptimize`, whose replacement takes no profile, so only baseline code changes: a regenerated baseline `negate` IC stops ORing observations into its own `m_arithProfile` pointer, and its UCB profile keeps learning (I16). History: [The `negate` operations](SPEC-image-history.md#the-negate-operations).

## 7. Baked facts

The baked facts are those THREAD Capture names: the capability class, the taint and the kind and depth of every unguarded scope access the compilation baked from its CB.

```cpp
namespace JSC::JITCache {
enum class ScopeOpcode : uint8_t { ResolveScope = 1, GetFromScope = 2, PutToScope = 3 };
struct ScopeFact {
    uint32_t bytecodeOffset;
    ScopeOpcode opcode;
    ResolveType resolveType;
    uint32_t localScopeDepth; // resolve_scope with ClosureVar only, else 0
};
struct BakedFacts {
    DFG::CapabilityLevel capabilityLevel;   // as read by the compilation
    bool couldBeTainted;
    Vector<ScopeFact> scopeFacts;           // sorted by bytecodeOffset, one per instruction at most
};
class BakedFactsBuilder { // the recorder's, filled on the compiling thread
public:
    void setCodeBlockFacts(DFG::CapabilityLevel, bool couldBeTainted);
    void addScopeFact(ScopeFact);    // in any order: the slow pass adds put_to_scope's ModuleVar fact after the main pass
    BakedFacts finish();             // sorts scopeFacts by bytecodeOffset; step 5 of section 4.7
};
}
```

Recording, on the compiling thread, through `ImageRecorder::bakedFacts()`:

- `JIT::compileAndLinkWithoutFinalizing` stores the `DFG::CapabilityLevel` it read from `m_profiledCodeBlock->capabilityLevel()` and `m_profiledCodeBlock->couldBeTainted()`.
- `JIT::emit_op_resolve_scope` adds a fact when it compiles `ModuleVar`, `ClosureVar` (with `m_localScopeDepth`) or `ClosureVarWithVarInjectionChecks`.
- `JIT::emit_op_get_from_scope` adds a fact when it compiles `ClosureVar`.
- `JIT::emit_op_put_to_scope` adds a fact when it compiles `ClosureVar`, `ResolvedClosureVar` or `ClosureVarWithVarInjectionChecks`; `JIT::emitSlow_op_put_to_scope` adds one when it compiles `ModuleVar`.

These are the shapes `compiler.md` lists as baked without a guard. The guarded shapes (global resolve types, `m_modeMetadata`, `m_enumeratorMetadata`, and `get_from_scope`'s `ClosureVarWithVarInjectionChecks`, which always reaches the C++ slow path) are correct for any CB and are not recorded.

Comparison, `compareBakedFacts(const ImageSectionsView&, CodeBlock& newborn) -> BakedFactsResult` (section 10.1), runs on the VM thread on a linked CB that has not run. It writes nothing to the CB (I13):

- The capability class matches when `level == DFG::CannotCompile` holds for both the recorded level and the CB's, where the CB's level is `capabilityLevelState()` when set and otherwise `computeCapabilityLevel()`, which does not memoize. The code depends on the class only through that test (N13).
- Taint matches when `couldBeTainted()` equals the recorded value exactly.
- Each scope fact matches when the CB's metadata for that instruction holds the same resolve type (`OpResolveScope::Metadata::m_resolveType` with `m_localScopeDepth` equal, or `m_getPutInfo.resolveType()`).

The result is `Match` or `Mismatch` naming the first differing fact. Every fact names an instruction of its own opcode: the producer's recording writes it so, and under strict U4 rejects a section that does not before comparison runs. History: [Baked facts](SPEC-image-history.md#baked-facts).

## 8. Sections

### 8.1 Encoding

The lane writes `image.baseline` and `baked-facts.baseline` for every captured body, and `image-twins.baseline` in `ENABLE(JITCACHE_TWINS)` builds (section 11.2). The container treats them as opaque bytes and hands each to the lane as a span that starts 8-byte aligned and covers exactly the section (R-INT-5). Integers and doubles are little-endian (`static_assert(std::endian::native == std::endian::little)`), read with `memcpy`-based loads. Regions start 8-byte aligned; padding and reserved fields are zero, which strict checks (section 8.5). Enumerations from the engine (`CommonJITThunkID`, `AccessType`, `CacheType`, `CallMode`, `ResolveType`, `DFG::CapabilityLevel`) are stored as their native values, which the header's build ID fixes. The header's CPU feature vector fixes the instruction forms emission chose (THREAD Storage). No field holds an address.

### 8.2 `image.baseline`

Header, 56 bytes. It carries no tag, layout, tier or architecture: the container's section directory gives each section its type and tier (SPEC-integrator.md section 6.1), and the header's build ID fixes the binary, hence the architecture and every layout. History: [Side tables and section headers](SPEC-image-history.md#side-tables-and-section-headers).

| offset | type | field |
|---|---|---|
| 0 | `u32` | `codeSize`, the linked size (section 3.1) |
| 4 | `u32` | `arityEntryOffset`, 0 when the image has no arity-check entry |
| 8 | `u32` | `fixupCount` |
| 12 | `u32` | `callCount` |
| 16 | `u32` | `moldCount` |
| 20 | `u32` | `simpleSwitchTableCount` |
| 24 | `u32` | `stringSwitchTableCount` |
| 28 | `u32` | `codeMapCount` |
| 32 | `u32` | `constantPoolCount` |
| 36 | `u32` | `mathICCount` |
| 40 | `u64` | `m_livenessRate`, IEEE bits |
| 48 | `u64` | `m_fullnessRate`, IEEE bits |

Regions, in order:

1. Code: `codeSize` bytes, canonical (section 8.4).
2. Fixups: `fixupCount` entries of 24 bytes: `u32 site`, `u8 form` (1 `Pointer`, 2 `Call`, 3 `Jump`), `u8 kind`, `u16` reserved, `u32 a`, `u32 b`, `i64 payload`. Kinds are numbered in the order of table 3.3, `Operation` = 1 through `SnippetEntry` = 21. In footprint order (section 3.2).
3. Calls (`m_unlinkedCalls`): `callCount` entries of 8 bytes: `u32` bytecode index bits (`BytecodeIndex::asBits`), `u32` offset of `doneLocation`. In the order `JIT::link` sorted them.
4. Molds (`m_unlinkedPropertyInlineCaches`), in mold order: `moldCount` entries of 16 bytes: `u8 accessType`, `u8 preconfiguredCacheType`, `u8` flags (bit 0 `propertyIsInt32`, 1 `propertyIsString`, 2 `propertyIsSymbol`, 3 `prototypeIsKnownObject`, 4 `canBeMegamorphic`), `u8` identifier kind (0 none, 1 UCB identifier, 2 immortal name), `u32` identifier (index, or `ImmortalName`: 1 `length`, 2 `next`, 3 `done`, 4 `value`, 5 `Symbol.hasInstance`, 6 `prototype`), `u32` bytecode index bits, `u32` offset of `doneLocation`.
5. Simple switch tables (`m_switchJumpTables`), one per UCB table in index order: `u32` offset of `m_ctiDefault`, which `JIT::link` sets for every table, list tables included, because `JIT::emit_op_switch_imm` and `JIT::emit_op_switch_char` append a `SwitchRecord` for each; `u32 entryCount` (0 for a list table); then `entryCount` `u32` offsets of `m_ctiOffsets`.
6. String switch tables (`m_stringSwitchJumpTables`), one per UCB table: `u32 entryCount`, the key count plus one, then the offsets of `m_ctiOffsets` by `m_indexInTable`, the default last.
7. Code map (`m_jitCodeMap`): `codeMapCount` `u32` code offsets, one per instruction start of the UCB in bytecode order. `JIT::link` builds the map from the label `JIT::privateCompileMainPass` sets at every instruction start and keeps no other entry (N27), so the bytecode indexes are the UCB's instruction starts and the section does not repeat them; install pairs them again (section 10.3, step 13).
8. Constant pool (`m_constantPool`): `constantPoolCount` entries of 8 bytes: `u32` type (0 `FunctionDecl`, 1 `FunctionExpr`), `u32` index.
9. MathICs, by MathIC index: `mathICCount` entries of 32 bytes: `u8` kind (1 add, 2 sub, 3 mul, 4 negate), `u8` flags (bit 0 `m_generateFastPathOnRepatch`, bit 1 has a snippet, bit 2 no inline code), `u16` reserved, `u32` bytecode offset, `u32 inlineStart`, `u32 inlineEnd`, `u32 slowPathStart`, `u32 slowPathCall` (the call's return point, `m_slowPathCallLocation`), `u32 snippetSize` (the snippet's linked size, section 3.1), `u32 snippetFixupCount`. An entry without inline code (section 6.1) has flags exactly bit 2 and every other field after the bytecode offset zero.
10. Snippets: for each MathIC with a snippet, by index: `snippetSize` canonical bytes, padded to 8, then `snippetFixupCount` fixup entries in footprint order, their sites relative to the snippet.

The section's size is a function of the header's counts and the table contents; a section of any other size is invalid.

### 8.3 `baked-facts.baseline`

Header, 8 bytes, with no tag, layout or tier for the reasons of section 8.2: `u8` capability level, `u8` taint (0 or 1), `u16` reserved, `u32 scopeFactCount`. Then `scopeFactCount` entries of 12 bytes: `u32` bytecode offset, `u8` opcode (`ScopeOpcode`), `u8` resolve type, `u16` reserved, `u32` local scope depth. Sorted by bytecode offset, one per instruction at most.

### 8.4 Canonical bytes

Capture writes the live image bytes `[0, codeSize)` and each snippet's `[0, snippetSize)`, except that inside each fixup's footprint it writes the canonical encoding of section 3.2. A section is therefore independent of addresses, and two captures of the same compilation with the same draws are equal (I8).

### 8.5 Validation

THREAD Session splits validation between normal mode and strict. In normal mode the container has verified each section's checksum in a body whose header and key matched, and this lane trusts the rest, which its own capture wrote (I2, I3, I8): `parseImageSections` only locates the regions the header's counts imply, and debug builds `ASSERT` what the checks below verify. Strict runs every check below, and a failed one is invalid material, since it guards an import. History: [Validation](SPEC-image-history.md#validation).

Every check has an identifier, and `ImageCheck` names the one that failed. History: [Check names](SPEC-image-history.md#check-names).

```cpp
namespace JSC::JITCache {
enum class ImageCheck : uint8_t {
    None,                       // an outcome no check names: NotEligible, ChargeRefused, ExecutableMemoryExhausted
    V1, V2, V3, V4, V5, V6, V7, // section 8.5, structure
    U1, U2, U3, U4, U5, U6, U7, // section 8.5, against the UCB
    W1, W2, W3, W4,             // section 11.2, twins builds
    S1, S2, S3, S4,             // section 9, capture
    S5,                         // section 10.3, step 10
};
ASCIILiteral description(ImageCheck); // names the failing step for status(vm): the check's name in lowercase, "v1" to "s5", or "none"
}
```

Structure, independent of the UCB (`parseImageSections` under strict):

- V1. Header fields: both coverage rates finite and within [0, 1], the ratios `CodeBlock::shouldOptimizeNowFromBaseline` computes; total size equal to the size the counts and tables imply; padding zero.
- V2. `codeSize` above 0; `arityEntryOffset` below `codeSize`; on ARM64 every code offset a multiple of 4.
- V3. Fixups: in footprint order (section 3.2), each footprint starting at or after the end of the one before it and lying inside its code (`[0, codeSize)` for the image, `[0, snippetSize)` for a snippet), with an x86_64 `Jump`'s length read from its opcode byte as table 3.2 says; every footprint holding exactly its form's canonical encoding (section 3.2), so each writer of section 10.3 finds the instruction it expects (N21); each form valid for its kind (table 3.3); unused fields zero; enum fields in range; `CodeSymbol` payloads valid (section 3.5); `ImageOffset` only in snippets and `SnippetEntry` only in the image.
- V4. Calls sorted by bytecode index; molds with valid access types, cache types, reserved flag bits zero and identifier kinds; switch, code-map and `doneLocation` offsets within the code; code-map offsets nondecreasing, as the main pass emits instructions in bytecode order; constant pool types 0 or 1.
- V5. MathICs: valid kinds and reserved bits zero. A MathIC without inline code has flags exactly bit 2, all four locations and both snippet fields zero, and no `SnippetEntry` fixup names it. Every other MathIC has bit 2 clear, `inlineStart < inlineEnd <= codeSize`, `slowPathStart` and `slowPathCall` within the code, and an `Operation` `Pointer` fixup at `farCallPointerSite(slowPathCall)`; its snippet fields are zero without a snippet; with a snippet it has exactly one `SnippetEntry` fixup naming it, at the jump site of its inline start (`inlineStart` on ARM64, `inlineStart + 5` on x86_64), and without one it has none; each of its snippet's `ImageOffset` targets equals its `inlineEnd` or `slowPathStart`.
- V6. `MathIC` and `SwitchTableBase` targets meet their conditions in table 3.3. A `MathIC` target must name a MathIC with inline code because only `JIT::emitMathICSlow` passes an IC's address.
- V7. Baked facts: a capability level of `DFG::CannotCompile`, `CanCompile` or `CanCompileAndInline` (the compilation reads it through `capabilityLevel()`, which never returns `CapabilityLevelNotSet`), a taint of 0 or 1, reserved zero, sorted unique offsets, valid opcodes and resolve types, depth zero except for a `resolve_scope` fact of type `ClosureVar`.

Against the UCB (`validateImageSectionsAgainst`, called only under strict), once the import's UCB exists:

- U1. Simple table count equals `UnlinkedCodeBlock::numberOfUnlinkedSwitchJumpTables()`; a dense table's entry count equals its unlinked `m_branchOffsets.size()` and a list table's is 0.
- U2. String table count equals `numberOfUnlinkedStringSwitchJumpTables()`; each entry count is the table's key count plus one, as natively: `BytecodeGenerator::beginSwitch` creates every table with the one `switch_string` that names it, and `JIT::emit_op_switch_string` sizes it with `ensureCTITable`. An empty table would put step 7 of section 10.3 and `operationSwitchStringWithUnknownKeyType` out of range.
- U3. UCB indexes in fixups, molds and the constant pool are below the UCB's constant, identifier, binary and unary arith profile, function declaration and function expression counts.
- U4. Bytecode indexes of calls, molds, MathICs and baked facts name instruction starts, with a checkpoint only where the instruction has one; a MathIC's instruction has its kind's opcode and a baked fact's instruction its opcode.
- U5. `codeMapCount` equals the UCB's number of instruction starts, so install pairs every instruction start with one offset (region 7; section 10.3, step 13) and no reader of the map meets a missing key (N12).
- U6. A table with rank fixups has exactly one `SwitchStringRankCase` fixup per rank and at least one `SwitchStringRankAtom` fixup per rank, and its rank fixups meet table 3.3's bounds.
- U7. Each `UCBConstantCell` and `UCBConstantAtom` target meets its kind's condition in table 3.3.

The UCB lane guarantees that the import's UCB has the producer's index spaces (R-UCB-1); under strict these checks catch a file that disagrees with them, not a different UCB. In twins builds the same two functions also run W1 to W4 (section 11.2).

## 9. Capture

```cpp
namespace JSC::JITCache {
enum class CaptureOutcome : uint8_t { NotEligible, ChargeRefused, RecordingFault };
struct CaptureFailure { CaptureOutcome outcome; ImageCheck check; }; // check: S1 to S4 for RecordingFault, else None

bool isImageCapturable(const BaselineJITCode&);

class ImageCapture {
    WTF_MAKE_NONCOPYABLE(ImageCapture);
public:
    ImageCapture(ImageCapture&&);
    size_t imageSectionSize() const;
    size_t bakedFactsSectionSize() const;
    [[nodiscard]] bool writeImageSection(const ScopedLambda<bool(std::span<const uint8_t>)>& sink) const;
    [[nodiscard]] bool writeBakedFactsSection(const ScopedLambda<bool(std::span<const uint8_t>)>& sink) const;
#if ENABLE(JITCACHE_TWINS)
    size_t twinsSectionSize() const;
    [[nodiscard]] bool writeTwinsSection(const ScopedLambda<bool(std::span<const uint8_t>)>& sink) const;
#endif
};

std::expected<ImageCapture, CaptureFailure> captureImage(VM&, CodeBlock&, BaselineJITCode&, ProducerBudget&, bool strict);
}
```

`isImageCapturable` holds when the `BaselineJITCode` carries a record in state `Complete`, which a record reaches only for shareable code (section 4.7, step 4). THREAD's other eligibility conditions (the CB is its executable's replacement and runs `BaselineJIT`) belong to the glue, which checks them (SPEC-integrator.md section 8.1), and so does the rule that capture runs only while production is active (R-INT-6).

`captureImage` runs on the VM thread with the API lock and heap access, JS paused and no collector phase on the thread (R-INT-6). It allocates no cell, drains nothing, and takes no JSC lock other than `JITThunks::m_lock`, which the native support lookups take when S1 or step 6 resolves a target; every such target already exists, because the compilation generated it. It reads only the `BaselineJITCode`, its record, its MathICs, its executable memory up to the linked sizes, and the UCB's identifiers, constants, arithmetic profiles and switch tables. Its steps:

1. Read the image's start from the `BaselineJITCode`'s code reference, its `codeSize` from the record, and the arity entry from `addressForCall(ArityCheckMode::MustCheckArity)`. The code reference's own size is the handle's, which can extend past the linked bytes (N20).
2. Convert every code pointer of the side tables to an offset: calls, molds, switch tables, and the code map, whose entries capture reads with `JITCodeMap::forEach` (section 14.1, N27). A pointer outside `[start, start + codeSize]` fails S4 under strict. With strict off, capture trusts that `JIT::link` placed every such pointer inside the code (THREAD Session), and debug builds `ASSERT` the range.
3. Classify each mold identifier: the index of a UCB identifier with the same `UniquedStringImpl` (through a map built once per capture and freed before `captureImage` returns), else the immortal name with that impl (`vm.propertyNames`), else the record becomes `Unrecordable(UnknownIdentifier)` and capture returns `NotEligible`. The capture keeps each mold's identifier field, its kind and value as region 4 encodes them, for the writes. Identifiers created by `createFromIdentifierOwnedByCodeBlock` and by `createFromImmortalIdentifier` carry the same bits for the same impl, so either encoding restores an identical mold.
4. For each MathIC with inline code, read its four locations and convert them to offsets as in step 2, then its flag and `m_code`. A MathIC with `m_code` must have provenance whose start equals that allocation's start and whose size is at most the allocation's handle size (`m_code.size()`); otherwise the record is `Unrecordable(MathICProvenance)` and capture returns `NotEligible`. A MathIC the record lists without inline code is written from the record alone: flags exactly bit 2 and zero fields (section 8.2).
5. Read the coverage rates with `livenessRate()` and `fullnessRate()`.
6. In twins builds, resolve every image and snippet fixup's target with `resolveTarget` in the capturing VM's context (the image start, the record's MathICs, the switch tables' storage, the string tables `JIT::link` filled, the provenance's snippet starts and `rankStringSwitches` of the UCB), giving the producer values that the twins section carries (sections 11.1 and 11.2).
7. Under strict, run S1 to S4. A failure is a recording fault (`CaptureOutcome::RecordingFault`), since each guards a capture (THREAD Session).

Capture charges every byte it allocates to the producer budget before the allocation (R-INT-2): step 3's map until `captureImage` returns, and what the `ImageCapture` keeps for the writes (the mold identifier fields of step 3 and, in twins builds, the producer values of step 6) until the `ImageCapture` is destroyed, which releases exactly what it still holds (I14). A refusal returns `CaptureOutcome::ChargeRefused` and the capture stops, releasing what it charged; the budget has already raised the recording fault (R-INT-2).

The write functions stream the section through the sink with no buffer of their own; the integrator's writer buffers what the sink receives (THREAD Execution). Nothing they read changes before JS resumes: only JIT operations regenerate MathICs, and only `CodeBlock::shouldOptimizeNowFromBaseline` rewrites the coverage rates, both while JS runs. In order: the header, built on the stack; the code, as alternating sink calls, the live bytes from one fixup footprint's end to the next one's start and then that footprint's canonical encoding (section 8.4, at most 12 bytes) from a stack array; the fixups and tables, each entry encoded on the stack as it is written, its code pointers converted to offsets as in step 2 and each mold's identifier field taken from step 3; then each snippet's `[0, snippetSize)` the same way as the code, with its fixups; padding comes from a static array of zeros. `imageSectionSize` follows from the header's counts, the string tables' entry counts and the snippet sizes, so nothing is encoded ahead of the writes. `writeTwinsSection` writes the record's twin data, the producer values of step 6 and this process's `captureProcessToken()`. They must run before JS resumes, while the capture's `BaselineJITCode` reference keeps the code alive (R-INT-6); a sink that refuses makes the write return false, which the writer reports as its own storage failure. History: [Capture](SPEC-image-history.md#capture).

Strict checks at capture:

- S1. Every image fixup decodes as its form (table 3.2 opcodes, the same `Xd` across a `Pointer`'s three instructions) and to the producer's resolution of its target. For a branch the decoded target must equal the resolved address, following unconditional `b` instructions through jump islands, at most as many as the pool's size (`endOfFixedExecutableMemoryPool<uintptr_t>() - startOfFixedExecutableMemoryPool<uintptr_t>()`) divided by half of `MacroAssembler::nearJumpRange`, rounded up, which bounds the chains `FixedVMPoolExecutableAllocator::islandForJumpLocation` builds for any pool size, including one the free option `jitMemoryReservationSize` sets.
- S2. The same for every snippet fixup.
- S3. `MathICHolder::forEachMathIC` (section 14.1) visits as many ICs of each kind as the record lists.
- S4. Side-table pointers, and the four locations of every MathIC with inline code, lie within `[start, start + codeSize]`, as in step 2; every MathIC the record lists without inline code still has null locations, no `m_code` and `m_generateFastPathOnRepatch` false.

## 10. Preparation at install

### 10.1 Interface

```cpp
namespace JSC::JITCache {
struct ImageSectionSpans {
    std::span<const uint8_t> image;
    std::span<const uint8_t> bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    std::span<const uint8_t> twins;
#endif
};
class ImageSectionsView; // spans into the borrowed payload; under strict, structurally valid (V1 to V7, and W1 to W3 in twins builds)

std::expected<ImageSectionsView, ImageCheck> parseImageSections(const ImageSectionSpans&, bool strict); // the V and W1 to W3 checks only under strict
std::expected<void, ImageCheck> validateImageSectionsAgainst(const ImageSectionsView&, const UnlinkedCodeBlock&); // U1 to U7, W4; strict only

enum class BakedFactsResult : uint8_t { Match, Mismatch };
BakedFactsResult compareBakedFacts(const ImageSectionsView&, CodeBlock& newborn);

enum class PrepareOutcome : uint8_t { InvalidMaterial, ExecutableMemoryExhausted };
struct PrepareFailure { PrepareOutcome outcome; ImageCheck check; }; // check: S5 for InvalidMaterial, else None

class PreparedImage {
    WTF_MAKE_NONCOPYABLE(PreparedImage);
public:
    PreparedImage(PreparedImage&&);
    const BaselineJITCode& code() const;                     // valid until commit or destruction
    Ref<BaselineJITCode> commit(VM&, CodeBlock& installing); // infallible; consumes the object; returns code()
};

std::expected<PreparedImage, PrepareFailure> prepareImage(VM&, UnlinkedCodeBlock&, const ImageSectionsView&, ProducerBudget*, bool strict); // not const: steps 2 and 13 call UCB accessors that have no const overload; a non-null budget rebuilds the record (step 14)
}
```

The glue calls these in THREAD Restoration's order under R-INT-7, and `prepareImage` and `commit` keep I11.

`code()` is the finished `BaselineJITCode` of section 10.3, step 13, which `commit` returns and setup installs, with the producer's molds, in mold order and with the producer's fields, in `m_unlinkedPropertyInlineCaches`. The other lanes' preparations read it before `commit` and write nothing to it; the ICs lane's `prepareBaselineICs` checks its mold pairing against it. Destroying the `PreparedImage` after a later preparation fails frees the code with everything else it holds and leaves the VM's statistics untouched. History: [Preparation and the rebuilt record](SPEC-image-history.md#preparation-and-the-rebuilt-record).

### 10.2 Context

`prepareImage` and `commit` run on the VM thread with the API lock and heap access, inside the install function's GC deferral, with no JSC-internal lock held on entry. They allocate no cell. Native calls take their own locks (`JITThunks::m_lock` in `VM::getCTIStub`, the executable allocator's locks, `FixedVMPoolExecutableAllocator::m_lock` when a writer places a jump island), and the lane never holds one lock while taking another. The borrowed payload needs to live only until `prepareImage` returns, and in twins builds until `Twins::checkImage` returns (R-INT-11): everything the prepared image keeps is copied out.

### 10.3 Steps

1. Validate nothing here. Validation is the glue's: under strict it runs before `compareBakedFacts`, which relies on U4 (R-INT-3, R-INT-7, section 8.5), and normal mode runs none. In both modes every footprint holds its form's canonical encoding (I8), which the writers of step 9 rely on. History: [Validation](SPEC-image-history.md#validation).
2. Create every MathIC, with inline code or without, in a local `MathICHolder`, with `addJITAddIC(&ucb.binaryArithProfile(op.m_profileIndex))` and the matching calls for sub, mul and negate, in MathIC index order; set each `m_generator` with `JIT::mathICGeneratorFor<Op>`, as `JIT::emitMathICFast` sets it for every IC, and `m_generateFastPathOnRepatch` from the flags.
3. Build the switch tables: `FixedVector<SimpleJumpTable>` and `FixedVector<StringJumpTable>` of the UCB's counts, each dense or string table's `m_ctiOffsets` sized from the section. Their storage addresses are final from here on; moving the outer vectors into the `BaselineJITCode` later does not move them.
4. Rank the string tables: `rankStringSwitches` of the consumer's UCB (section 3.8), which sorts each inline-tree table's keys by signed address, giving rank to key and to `m_indexInTable`.
5. Allocate the image with `ExecutableAllocator::singleton().allocate(codeSize, JITCompilationCanFail)`, and each snippet with its `snippetSize` the same way; a null result is `ExecutableMemoryExhausted`. A handle can be larger than requested (N20); its slack stays unwritten, as natively.
6. Copy the image's `codeSize` bytes and each snippet's `snippetSize` bytes once with `performJITMemcpy`.
7. Fill the switch tables' code locations, now that the image's start is known: every simple table's `m_ctiDefault`, list tables included, as `JIT::link` sets it, each dense table's `m_ctiOffsets`, and each string table's `m_ctiOffsets` default included, at the image start plus the section's offsets. `SwitchStringRankCase` targets read them in step 8, as the producer's resolution read the tables `JIT::link` filled (section 3.7).
8. Resolve every fixup target with `resolveTarget` and the consumer's context (image start, the MathICs of step 2, the table storage of step 3 with the code locations of step 7, the ranks of step 4, the snippet starts of step 5).
9. Patch every snippet fixup, then every image fixup, with its form's writer (table 3.2) on the executable memory. Since each footprint holds its form's encoding (step 1), each writer writes only inside its footprint, apart from the jump islands the ARM64 writers allocate (N21).
10. Under strict, S5, the read-back after patching: each patched footprint decodes, as S1 does with islands followed, to the consumer's resolution of its target. S5 guards an installation, so a failure is invalid material.
11. Flush the instruction cache over `[start, start + codeSize)` and each snippet's `[start, start + snippetSize)` with `MacroAssembler::cacheFlush`, as `LinkBuffer::performFinalization` does over the linked size.
12. For each MathIC with inline code, set its `m_inlineStart`, `m_inlineEnd`, `m_slowPathStartLocation`, `m_slowPathCallLocation` and, when it has a snippet, `m_code` (a `MacroAssemblerCodeRef<JITStubRoutinePtrTag>` over the snippet's handle). A MathIC without inline code keeps the null locations and empty `m_code` it was created with, as native emission leaves such an IC.
13. Construct the `BaselineJITCode` from the image's handle (retagged to `JSEntryPtrTag`) and its arity entry, then set `m_unlinkedCalls`, the molds (identifiers through `CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(&ucb, ucb.identifier(i))` or `createFromImmortalIdentifier`), both switch tables, `m_jitCodeMap` through `JITCodeMapBuilder`, which appends the UCB's instruction starts in bytecode order, each with the section's next code-map offset (U5 checks the counts agree under strict, and debug builds `ASSERT` it), `m_constantPool`, `adoptMathICs`, `m_isShareable = true`, both coverage rates through their setters, and no `PCToCodeOriginMap`. This is the object `PreparedImage::code()` returns.
14. When the budget is non-null (a ConsumerProducer VM whose production is active, R-INT-7), build the record THREAD Capture describes from the section (`codeSize`, image fixups, MathIC records with the new IC pointers and each snippet's start, size and fixups, an empty slow-call site for each MathIC without inline code, baked facts) and, in twins builds, from the twins section the twin data of section 11.1 (seeds, compile inputs and the regeneration log). Charge the record's exact bytes to the budget, with its containers sized from the section's counts, and attach it. A refusal leaves the image without a record, and the import proceeds; the budget raises the recording fault (R-INT-2). History: [Preparation and the rebuilt record](SPEC-image-history.md#preparation-and-the-rebuilt-record).

`commit` adds the code's `size()`, which is its handle's size, divided by `ucb.instructionsSize()` to `VM::machineCodeBytesPerBytecodeWordForBaselineJIT`, as `JIT::finalizeOnMainThread` adds `jitCode->size()` for a native compilation. It then issues `WTF::crossModifyingCodeFence()`, reports the code to `PerfLog` and `GdbJIT` under `useJITDump` and `useGdbJITInfo` with the name `LinkBuffer::logJITCodeForJITDump` would give a baseline compilation of that CB, and returns the code, so that flush, fence and code-size sample follow native finalization (THREAD Restoration).

Once native setup installs it, the image is native: later CBs of the body install it through doors 2 and 3, its MathICs regenerate natively (recorded in a ConsumerProducer VM), and `CodeBlock::shouldOptimizeNowFromBaseline` rewrites its coverage rates.

## 11. Twins

### 11.1 Twin data

`ENABLE(JITCACHE_TWINS)` builds record what the twins need (THREAD Verification). The record keeps it as its twin data (section 5), capture writes it to `image-twins.baseline` (section 11.2) together with the producer values, and the import reads it back for the check (section 11.3) and, in a ConsumerProducer VM, for the rebuilt record. The twin data has three parts. History: [The twin CB and its inputs](SPEC-image-history.md#the-twin-cb-and-its-inputs).

Seeds. The compilation's assembler seed, which every baseline compilation draws, since `JIT::compileAndLinkWithoutFinalizing` calls `random()` for its entry `nop`, and which `AbstractMacroAssemblerBase::initializeRandom` reports through `didInitializeRandom`; and the seed of each `BinarySwitch`, in construction order, which `binarySwitchSeed` records.

Compile inputs. Every value baseline emission reads from mutable state. The capability level and the taint already travel as baked facts (section 7). The CB's metadata and the UCB's arithmetic profiles come from `snapshotCompileInputs(CodeBlock&)` (`ImageTwins.cpp`). `JIT::compileAndLinkWithoutFinalizing` calls it once the recorder is attached and before the main pass, and hands the result to `ImageRecorder::recordCompileInputs`. It walks the UCB's instructions and reads, through each instruction's `metadata(CodeBlock*)` accessor on the CB being compiled:

- `m_resolveType` of every `resolve_scope`, and `m_localScopeDepth` when that type is `ClosureVar` (read by `JIT::emit_op_resolve_scope` and `JIT::emitSlow_op_resolve_scope`);
- `m_getPutInfo.resolveType()` of every `get_from_scope` and `put_to_scope` (`JIT::emit_op_get_from_scope`, `emitSlow_op_get_from_scope`, `emit_op_put_to_scope`, `emitSlow_op_put_to_scope`), which the emitters read at global sites as well as at the baked kinds;
- `m_modeMetadata.mode` of every `get_by_id`, `iterator_open` and `async_iterator_open` (`JIT::emit_op_get_by_id` and the iterator-open emitter in `JITCall.cpp`);
- `m_enumeratorMetadata` of every `enumerator_next` (`JIT::emit_op_enumerator_next`);

and the bits of every binary and unary arithmetic profile of the UCB.

The last input is the atom-ness of the UCB's string constants at the strict-equality instructions (N25). Since atom-ness only grows, it cannot be written into the twin like the others; the templates record the choice where they make it, and the twin compile reads it back there. `JIT::compileOpStrictEq` and `JIT::compileOpStrictEqJump` pass the operand their `tryGetAtomStringConstant` lambdas chose (`None`, `Lhs` or `Rhs`) through `ImageRecorder::strictEqualityAtomOperand` whenever a recorder is attached. A recording compilation's recorder stores it as a compile input of kind 8 and returns it, and `finishBaselineCompile` merges these inputs, which arrive in bytecode order, into the snapshot's by bytecode offset. A twin recorder returns the producer's input for the instruction instead, and the template emits the fast path for the operand it names, taking that constant's string from the UCB, or the generic comparison for `None`. Recorded at the read, the input is exact on whichever thread the producer compiled, and W4 guarantees that the twin finds an input at every instruction that asks and an atom at every operand an input names.

Emission reads nothing else that changes: the other `m_profiledCodeBlock` reads are layout the UCB fixes (`metadataTable()->offsetInMetadataTable`, `isConstantOwnedByUnlinkedCodeBlock`, `scopeRegister`); the other constant properties emission tests (a representation, a cell's pointer, an int32, `undefined`, `null` or a boolean) never change; the profiler opcodes leave their bodies unrecordable; and the operand of a scope access is loaded at run time.

The regeneration log. An active native regeneration (section 6.2) appends one entry to the record's regeneration log when it is constructed, whatever then happens to its snippets: the MathIC index, `profileBitsAtEntry`, and `callReplacement` as a `CodeSymbol` (a replacement outside the text segment makes the record `Unrecordable(ForeignCodeSymbol)`). Each `attach` adds a slot to that entry, where the recorder it attaches stores the assembler's seed if the assembler draws (`didInitializeRandom`); otherwise the slot stays empty. Outside twins builds the `MathICRegeneration` constructor ignores its last two arguments.

The producer values the relocation clause (section 11.4) needs are not part of the record: capture computes them by resolving every fixup in the capturing VM (section 9, step 6). In a ConsumerProducer VM the rebuilt record takes its twin data from the imported twins section (section 10.3, step 14), and its later native regenerations append to that log, so the next capture carries the original compilation's seeds and inputs and every regeneration since, and a fresh consumer's twin replays them all (T15).

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache {
struct TwinSeeds {
    uint32_t assembler;                // every baseline compilation draws it for its entry nop
    Vector<uint32_t> binarySwitches;   // in construction order
};
enum class CompileInputKind : uint8_t {
    ResolveScopeType = 1, GetFromScopeType, PutToScopeType, GetByIdMode, IteratorOpenMode, AsyncIteratorOpenMode, EnumeratorMetadata,
    StrictEqualityAtomOperand,
};
enum class StrictEqualityAtomOperand : uint8_t { None = 0, Lhs = 1, Rhs = 2 }; // the operand compared inline by its atom (N25)
struct CompileInput {
    uint32_t bytecodeOffset;
    CompileInputKind kind;
    uint8_t value;            // a ResolveType, a GetByIdMode, the enumerator byte or a StrictEqualityAtomOperand
    uint32_t localScopeDepth; // ResolveScopeType of type ClosureVar only, else 0
};
struct TwinCompileInputs {
    Vector<CompileInput> inputs;    // sorted by bytecode offset, one per instruction
    Vector<uint16_t> binaryArithBits;
    Vector<uint16_t> unaryArithBits;
    bool compiledHoldingAPILock;    // no JS of the VM ran while the compilation read these inputs (section 11.2, flag bit 0)
};
struct TwinRegeneration {
    uint32_t mathICIndex;
    uint16_t profileBitsAtEntry;
    CodeSymbol replacement;
    Vector<std::optional<uint32_t>, 2> assemblerSeeds; // one slot per attach, in attach order
};
struct TwinData {
    TwinSeeds seeds;
    TwinCompileInputs compileInputs;
    Vector<TwinRegeneration> regenerations;
};

TwinCompileInputs snapshotCompileInputs(CodeBlock&); // compile thread, before the main pass

// This process's token, 16 bytes drawn once with cryptographicallyRandomValues at the first call, redrawn while all zero.
// Any thread; the first call is serialized with std::call_once.
std::span<const uint8_t, 16> captureProcessToken();
}
#endif
```

### 11.2 The `image-twins.baseline` section

Only `ENABLE(JITCACHE_TWINS)` builds write this section. The header's build ID ties an artifact to one binary, so a twins build reads only bodies a twins build wrote: it always receives the section, and a body without it is invalid material there. The section carries the twin data of section 11.1 and what the relocation clause of section 11.4 needs: the producer values and the token of the process that captured the body.

Header, 48 bytes, with no tag, layout or tier for the reasons of section 8.2:

| offset | type | field |
|---|---|---|
| 0 | `u8` | flags: bit 0, the compilation ran on the thread holding the VM's API lock, so its compile inputs equal what emission read (section 11.3) |
| 1 | `u8[3]` | reserved |
| 4 | `u32` | the assembler's seed, which every baseline compilation draws for its entry `nop` (`random()` in `JIT::compileAndLinkWithoutFinalizing`) |
| 8 | `u32` | `binarySwitchSeedCount` |
| 12 | `u32` | `inputCount` |
| 16 | `u32` | `binaryArithProfileCount` |
| 20 | `u32` | `unaryArithProfileCount` |
| 24 | `u32` | `regenerationCount` |
| 28 | `u32` | `producerValueCount` |
| 32 | `u8[16]` | the capture-process token: `captureProcessToken()` of the process whose VM captured the body (section 11.1) |

Regions, in order, each starting 8-byte aligned:

1. `BinarySwitch` seeds: `binarySwitchSeedCount` `u32`, in construction order.
2. Compile inputs: `inputCount` entries of 12 bytes, sorted by bytecode offset, one per instruction: `u32` bytecode offset, `u8` kind (1 `resolve_scope` type, 2 `get_from_scope` type, 3 `put_to_scope` type, 4 `get_by_id` mode, 5 `iterator_open` mode, 6 `async_iterator_open` mode, 7 `enumerator_next` metadata, 8 strict-equality atom operand), `u8` value (a `ResolveType`, a `GetByIdMode`, the enumerator byte, or a `StrictEqualityAtomOperand`: 0 none, 1 the left operand, 2 the right), `u16` reserved, `u32` local scope depth (kind 1 of type `ClosureVar`, else 0).
3. Binary arithmetic profile bits: `binaryArithProfileCount` `u16`, by profile index.
4. Unary arithmetic profile bits: `unaryArithProfileCount` `u16`, by profile index.
5. Regeneration log, in regeneration order: `regenerationCount` entries, each a `u32` MathIC index, the `u16` profile bits at entry, a `u8` attach count (1 or 2), a `u8` seed mask (bit `i` set when attach `i`'s assembler drew), the `i64` replacement `CodeSymbol`, then one `u32` seed per attach (0 where the mask bit is clear), padded to 8.
6. Producer values: `producerValueCount` `u64`, the address each fixup's target resolved to in the capturing VM (section 9, step 6): the image's fixups in footprint order, then each snippet's in MathIC index order and footprint order.

A regeneration attaches at most two assemblers, the fast-path snippet's and the full snippet's (`JITMathIC::generateOutOfLine`), and at least one, since every path attaches the snippet it builds before emitting it; the inline rewrite attaches none (section 6.2).

Structure, independent of the UCB (`parseImageSections` under strict):

- W1. Reserved bytes and bits zero, the capture-process token not all zero; total size equal to the size the counts imply; padding zero.
- W2. Inputs sorted by strictly increasing bytecode offset; kinds 1 to 8; a value that is a valid `ResolveType` for kinds 1 to 3, a valid `GetByIdMode` for kinds 4 to 6, within `JSPropertyNameEnumerator`'s flag bits for kind 7, and at most 2 for kind 8; the depth zero except for kind 1 of type `ClosureVar`.
- W3. Regenerations: each MathIC index below `mathICCount` and naming a MathIC with inline code; attach counts 1 or 2; mask bits only below the attach count; seeds zero where the mask bit is clear; replacements valid `CodeSymbol`s. `producerValueCount` equals the image's `fixupCount` plus every snippet's `snippetFixupCount`.

Against the UCB (`validateImageSectionsAgainst` under strict):

- W4. Each input's bytecode offset names an instruction start whose opcode matches its kind (kind 8: `stricteq`, `nstricteq`, `jstricteq` or `jnstricteq`), and every `resolve_scope`, `get_from_scope`, `put_to_scope`, `get_by_id`, `iterator_open`, `async_iterator_open` and `enumerator_next` of the UCB has exactly one input. Every strict-equality instruction whose template reaches its atom test, which is every one neither of whose operands is a constant the UCB owns holding `undefined`, `null` or a boolean (N25), has exactly one input of kind 8, and no other instruction has one. An input of kind 8 with value 1 or 2 names an operand that is a constant the UCB owns and a string whose `tryGetValueImpl()` exists and is an atom: atom-ness only grows, so the operand the producer compared inline is an atom here unless the section disagrees with its UCB (R-UCB-1). The two arithmetic counts equal the UCB's.

A failure is invalid material, as for the checks of section 8.5, since in a twins build this section is part of what the import receives. Twins runs are tests, which run with strict on (THREAD Verification), so the twin check always reads validated twin data.

### 11.3 The check

The check has two preconditions, one per process. In the producer, the snapshot equals what the emitters read only when no JS of the VM ran while the compilation read its inputs, which holds exactly when the compilation ran on the thread holding the VM's API lock: every baseline compilation with `useConcurrentJIT` off (`JITWorklist::enqueue` then compiles and finalizes on the calling thread), and those `JIT::compileSync` runs. `snapshotCompileInputs` records it from `VM::currentThreadIsHoldingAPILock`, and the twins section carries it as flag bit 0. In the consumer, steps 3 and 5 below overwrite the UCB's arithmetic profiles, which compiler threads read without a lock (`ByteCodeParser::makeSafe` for any CB of the body or for a caller that inlines it, and a baseline compile of another CB of the body), so the consumer must run with `useConcurrentJIT` off, where no plan compiles on a worklist thread. `checkImage` tests both first, and the `TwinReport` keeps the skips it reports apart from differences (R-INT-11). With `useConcurrentJIT` off in every process of a run, both preconditions hold for every import, so a skip there is a defect and fails the run (THREAD Verification), and twins runs turn the option off in every process (R-INT-11, T2). History: [When the twin check runs](SPEC-image-history.md#when-the-twin-check-runs).

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache {
class TwinReplay; // one recorded regeneration: entry bits, replacement, assembler seeds in attach order, a failure flag

class Twins { // one per VM, owned by the integrator (R-INT-11); no state, since each twin dies after its check
public:
    void checkImage(VM&, CodeBlock& installed, JSScope*, const BaselineJITCode& restored,
        const ImageSectionsView&, TwinReport&); // the view carries the parsed twins section
};

// The process-wide registry of live twin CBs, under its own lock; set and lock are never destroyed (step 2).
void rememberImageTwin(CodeBlock&);
bool forgetImageTwin(CodeBlock&); // erases the CB and returns whether it was a twin; CodeBlock::~CodeBlock calls it first
size_t imageTwinCountForTesting(); // T21

// Test hooks of section 16.1: set before the process's first VM, read-only afterward.
enum class ImageTestHook : uint8_t { None, RelocationPairs, OperationPair, ChangeRecordedTarget, SkipPatch };
void setImageTestHook(ImageTestHook);
ImageTestHook imageTestHook();
}
#endif
```

`checkImage` runs on the VM thread at the end of `ScriptExecutable::prepareForExecutionImpl`, after `installCode` and before JS runs (R-INT-11). When `Options::useConcurrentJIT()` is on or the twins section's flag bit 0 is clear, it creates no twin, compares nothing, reports a skip naming the precondition that failed and returns. Otherwise:

1. Twin CB. Under its own `DeferGCForAWhile`, create a fresh CB of the installed CB's class through that class's `create(vm, executable, unlinkedCodeBlock, scope)` (`FunctionCodeBlock`, `ProgramCodeBlock`, `ModuleProgramCodeBlock` or `EvalCodeBlock`), with the installed CB's owner executable and UCB and the scope `prepareForExecutionImpl` received. This is native linking of one more CB of the body. The check never uses `ScriptExecutable::newCodeBlockFor`, whose slot for the kind `installCode` has just filled, nor `newReplacementCodeBlockFor`, whose `CopyParsedBlock` constructor shares the installed CB's `m_metadata` (N18). The twin has its own metadata table, never runs, never receives JIT code and is never installed. A creation that returns null or throws is reported, its exception cleared, and the check stops.
2. Lifetime. The check holds the twin in a local `Strong<CodeBlock>` until it returns, and registers it with `rememberImageTwin` as soon as `create` returns it. Nothing else reaches the twin, since no executable's code-block edge points to it, and a later collection sweeps it like any unreachable CB. What a visit of it marks (`CodeBlock::stronglyVisitStrongReferences` appends `m_unlinkedCode`, `m_ownerExecutable` and `m_globalObject`) is what the installed CB already holds, so the twin keeps no UCB, executable or realm alive past the check, and UCBs, realms and the installed CB die as they would without the check. Its destructor must not write `didOptimize` (N18), UCB feedback that travels, since natively no such CB exists. So the twins-only edit of section 14.1 makes `CodeBlock::~CodeBlock` call `forgetImageTwin(*this)` before anything else, which erases the CB from the registry and returns whether it was there, and skip the `didOptimize` write for a twin. The registry is one set of CB pointers for the process, in `ImageTwins.cpp`, under its own lock, since two VMs may sweep their heaps on different threads at once. Neither is ever destroyed, because `CodeBlock::~CodeBlock` can run during process exit: the set is a function-local `static NeverDestroyed<HashSet<CodeBlock*>>` that `imageTwinRegistry()` returns, which the first `forgetImageTwin` or `rememberImageTwin` creates from whichever thread calls first, and the lock is a namespace-scope `static Lock imageTwinRegistryLock`, constexpr-constructed with no destructor. An entry leaves the set exactly when its CB is destroyed, during shutdown included, so an address a twin held never answers for a later CB. By N18, nothing else in the twin's destruction reaches state other code reads. History: [The twin's lifetime](SPEC-image-history.md#the-twins-lifetime).
3. Inputs. Write each recorded compile input of kinds 1 to 7 into the twin's own metadata through its instruction's `metadata(CodeBlock*)` accessor, leaving every entry in a state the GC reads safely, since a collection may visit and finalize the twin while anything still reaches it, a conservative stack root included (`CodeBlock::reconcileLLIntInlineCachesAtGCEnd`); the inputs of kind 8 reach the compile through the twin recorder (step 4):
   - `resolve_scope`: `m_resolveType`, and `m_localScopeDepth` for `ClosureVar`. The GC reads only the entry's cell union there, which linking filled with a cell or null whatever the type.
   - `get_from_scope` and `put_to_scope`: `m_getPutInfo` rebuilt from the recorded type and the twin's own resolve mode, initialization mode and ECMA mode, with the structure-or-watchpoint-set union cleared and `m_operand` zero. For every type except the variable kinds the GC reads that union as a `StructureID`, and a watchpoint-set pointer left by another type would decode as garbage.
   - `get_by_id`, `iterator_open`, `async_iterator_open`: for a recorded `ProtoLoad`, the `protoLoadMode` fields all zero, which sets the mode to `ProtoLoad` and the hit count to zero; for any other mode, `clearToDefaultModeWithoutCache()`. The emitters test only `mode == GetByIdMode::ProtoLoad`. The GC reads a structure ID only from a `Default` entry, here zero, and reaches a `ProtoLoad` entry only through `m_llintGetByIdWatchpointMap`, which is empty for the twin.
   - `enumerator_next`: the recorded byte.

   Check that the twin's `capabilityLevel()` is `CannotCompile` exactly when the recorded level is, and that its `couldBeTainted()` equals the recorded taint, reporting any difference. Both follow from the executable and source provider the twin shares with the installed CB, and the code depends on the level only through that test (N13), so the twin compiles with the producer's class and no test hook writes its private `m_capabilityLevelState`. Save the UCB's arithmetic profiles in a scope object that writes them back on every exit from the check, and overwrite them with the recorded compile-start bits through `ArithProfile::restoreBits` (R-UCB-4).
4. Compile. Create `adoptRef(*new BaselineJITPlan(twin))` and `JIT jit(vm, plan, twin)`, call `jit.setJITCacheTwin` with the unlimited budget of R-INT-2, the recorded `TwinSeeds` and the recorded compile inputs, and call `jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail)` directly, without the plan's profile drain, which the baseline does not read, and without finalizing. The twin recorder seeds the assembler when `attachTo` attaches it, hands each `BinarySwitch` its recorded seed and answers each strict-equality template with the recorded operand, so emission draws what the producer drew and compares inline where the producer did. The result is the twin `BaselineJITCode`, with the record `finishBaselineCompile` built for it as for a producer's compilation. A null result (no executable memory) is reported and the check stops.
5. Replays. For each recorded regeneration, in order, overwrite the IC's profile with the recorded entry bits and call the twin IC's `generateOutOfLine(twin, replacement, regeneration)`, with the replacement resolved from its `CodeSymbol` and a twin `MathICRegeneration` (section 6.2) bound to the twin's record and to a `TwinReplay` that holds that regeneration's slots. Each `attach` seeds its assembler with the next slot's seed before anything is emitted, and leaves the assembler unseeded when the slot is empty. `generateOutOfLine` derives `shouldEmitProfiling` from the twin's JIT type, which is not an optimizing tier, as it did for the producer's baseline CB, and `ftlThunkAwareRepatchCall` repatches the twin's slow call natively. A replay that fails to allocate is reported, and no fault is raised.
6. Restore the UCB's arithmetic profiles from step 3's scope object, which restores them the same way when the check stops earlier, as at a null compile in step 4.
7. Compare, as section 11.4 says, then drop the twin `BaselineJITCode`, which frees its code, its snippets and its record.

### 11.4 The comparison

The restored image and its twin are two allocations at different addresses. Every `Call` and `Jump` fixup, the `ImageOffset` jumps of snippets included, is PC-relative and holds a different displacement in each, and on ARM64 it may go through different islands; artifact targets (`MathIC`, `SwitchTableBase`, `SnippetEntry`, `ImageOffset`, `SwitchStringRankCase`) have different values by construction. So the comparison reads bytes only outside footprints and fixups by logical target. History: [Comparing an image with its twin](SPEC-image-history.md#comparing-an-image-with-its-twin).

- Sizes and bytes. The twin record's `codeSize` equals the section's, and each twin snippet's linked size its `snippetSize`. The canonical form of the twin's image and of each twin snippet (its linked bytes with every footprint of the twin's record in canonical encoding, section 8.4) equals the section's code and snippet bytes. By I10 the restored bytes equal the section's outside footprints, so the restored code equals its twin byte for byte everywhere else. No side reads past a linked size, where each allocation keeps the pool's earlier content (N20).
- Fixups. The twin record's fixup lists, for the image and for each snippet, equal the section's entry by entry: site, form, kind and fields. The fields name logical targets (keys, indexes, offsets), never addresses.
- Decodes. Every footprint of the restored image and snippets decodes, as S1 does with islands followed, to the resolution of its target in the restored context (the restored image, its MathICs, tables, snippets and string ranks), and every footprint of the twin decodes to the resolution in the twin's context. Support, VM, UCB, process and structure-base targets resolve to the same value in both contexts, so for them this also compares the absolute targets; artifact targets compare by logical target only.
- Tables and states. The side tables as offsets, the molds (fields and identifier impls), each MathIC's state (with or without inline code, `m_generateFastPathOnRepatch`, locations as offsets, snippet presence) and the baked facts equal the twin's. Generators need no comparison: `JIT::emitMathICFast` and the import build them with the same function (section 6.3).
- Rates. The restored code's coverage rates equal the capture's, state the engine cannot recompute, which THREAD compares with its capture record.
- Relocation. For every fixup the clause compares, the producer value from the twins section differs from the target's resolution in the restored context, and each equal pair is reported with the relocation domain that resolution lies in (below). A reference missing from the record keeps the producer's value in the restored image and takes the consumer's in the twin, so the byte comparison above finds it only where those values differ, which THREAD Verification secures by computing twins "with every process, VM, UCB, support and Structure-reservation address differing between producer and consumer". The clause compares no pair for three kinds of fixup:
  - one with an artifact target, whose addresses are the two processes' own allocations, which THREAD Verification does not ask to differ; the decodes compare it by logical target;
  - one whose target lies "inside an object loaded at its link-time address", which THREAD Verification's relocation requirement skips: its resolution lies in a loaded segment of an object whose load bias is zero, as `dl_iterate_phdr` reports the objects, read once per process as section 3.5 reads the text segment. The domain table's engine-image row names the targets this skips under Bun's build flags (N23; history: [Relocation domains](SPEC-image-history.md#relocation-domains));
  - every fixup of a body whose twins section carries the importing process's own `captureProcessToken()`, since the requirement also "skips every target of a body the importing process captured itself": this process captured it, by this VM or another, so no domain can have moved between its capture and its import. A process forked from another inherits its token with the address space it describes. Every comparison above still runs for such a body. History: [Bodies a process captured itself](SPEC-image-history.md#bodies-a-process-captured-itself).

Each difference goes to the `TwinReport`.

For a body another process captured, the relocation clause can pass only when every domain it compares has moved between that process and the process that imports it. A domain is a region of memory that moves as a whole, and a target belongs to the domain its resolution lies in. Each kind but the three atom kinds has one domain; the check places an atom in the engine image when it lies in a loaded segment of the object holding `codeSymbolAnchor`, which `dl_iterate_phdr` lists as section 3.5 does, and in the heap otherwise:

| domain | targets that resolve into it | what moves it |
|---|---|---|
| engine image | every `Operation` target, and every `UCBConstantAtom`, `UCBIdentifier` or `SwitchStringRankAtom` target that names a static `StringImpl`, such as the empty string's `StringImpl::s_emptyAtomString`, the atom of every `""` constant (N24) | its load address, which Bun's flags fix at the link-time address (N23), so the clause skips these targets |
| executable pool | every `CommonThunk`, `BaselineThunk`, `SlowPathThunk`, `InlineCacheSlowPathThunk`, `VirtualCallThunk` and `ProcessThunk` target (both process thunks live in the pool, N22) | the runner places the two pools in disjoint ranges (R-INT-11) |
| structure reservation | the `StructureIDBase` target | the runner places the two reservations at different bases (R-INT-11) |
| heap | every other target: the VM's fields (`VMAddress`), cells (`VMCell`, `UCBConstantCell`), the other atoms and identifiers, and the UCB's arithmetic profiles | address randomization of the space the heap's allocator maps; the runner's check that it moves, its fallback where the allocator keeps a fixed base, and its repetition of a run whose only reports are equal pairs here are R-INT-11's |

## 12. Interfaces

### 12.1 What this lane exports

| consumer | what | section |
|---|---|---|
| integrator, capture glue | `isImageCapturable`, `captureImage`, `ImageCapture` | 9 |
| integrator, install glue | `parseImageSections`, `validateImageSectionsAgainst`, `compareBakedFacts`, `prepareImage`, `PreparedImage::code`, `PreparedImage::commit` | 10 |
| integrator, status | `ImageCheck` and its `description(ImageCheck)` | 8.5 |
| integrator, option table | the rows of options.md that name this lane | 1 (item 8), R-INT-9 |
| ICs lane | `const BaselineJITCode& PreparedImage::code() const`, the argument its `prepareBaselineICs(std::span<const uint8_t>, const BaselineJITCode&, CodeBlock&, StrictChecks)` receives before `commit` | 8.2, 10.1 |
| every part editing baseline emitters | the helpers of section 4.3 | 4.3 |
| test harness | `JITCache::Twins` | 11.3 |

### 12.2 Requirements on other parts

On the UCB lane:

- R-UCB-1. The UCB passed to `validateImageSectionsAgainst`, `compareBakedFacts` and `prepareImage` has the producer's index spaces, which THREAD Storage has the UCB lane guarantee: the same instruction stream (opcodes, operands, widths, checkpoints, so the same bytecode offsets and metadata layout) and out-of-line jump targets, which `JIT::jumpTarget` reads through `outOfLineJumpOffset` for a jump whose operand is 0; the same constant registers, with the same `SourceCodeRepresentation`, cells of the same type and content at the same indexes; the same identifiers; the same binary and unary arith profile counts; the same unlinked simple switch tables (`m_min`, `m_isList`, `m_branchOffsets`, `m_defaultOffset`); the same unlinked string switch tables (keys, and per key `m_branchOffset` and `m_indexInTable`); the same function declaration and expression counts, with each entry's builtin, arrow and strict bits, from which `selectNewFunctionOperation` picks the operation `new_func` and `new_func_exp` call; the same exception handlers; and the same `numParameters`, `numCalleeLocals`, `numVars`, scope register (which `JIT::emit_op_enter` compares and passes to `emitGetScope`), `codeType` and `isConstructor`. The image section records none of these, so no U check compares them. Beyond the index spaces, the lane relies only on THREAD Restoration's atom contract, which the UCB lane keeps (SPEC-ucb.md section 9.1), for the constant of each `UCBConstantAtom` fixup and of each strict-equality input that names an operand (section 11.1, N25).
- R-UCB-2. The import's UCB exists and the borrowed payload is alive when the glue calls this lane's install functions.
- R-UCB-3. `UnlinkedCodeBlock::numberOfBinaryArithProfiles()` and `numberOfUnaryArithProfiles()`, which the UCB lane adds, for U3 and for `ImageRecorder::arithProfileTarget`.
- R-UCB-4. `ArithProfile::restoreBits`, which the UCB lane adds for its seeds, for the twin check's save, overwrite and restore of the UCB's arithmetic profiles (section 11.3).

On the ICs lane, and on every part that edits code emitted into a baseline image or MathIC snippet:

- R-ALL-1. A value such an edit embeds that depends on the process, the VM, a UCB or another allocation goes through a helper of section 4.3, and every new thunk link through `nearCallSupport`, `jumpSupport` or `linkJumpToSupport`. An edit inside a header JSC exports reaches recording only through the hooks of section 4.3 and includes no `jitcache/` header (section 14.4). The ICs lane's `super_construct` change in `JIT::compileOpCall` keeps `JSCell::seenMultipleCalleeObjects()` a literal and adds no reference.
- R-ICS-1. Property-IC state is indexed by mold index, which this lane preserves; the ICs lane attaches it after native setup builds `BaselineJITData` from the restored molds.

On the integrator (the owner of these interfaces; names are working names):

- R-INT-1. `JITCache::ProducerContext* JITCache::producerContext(VM&)`, callable from any thread without a lock, with `Ref<ProducerBudget> budget()`. It is non-null exactly while the VM's production is active: the VM has a producing role, its cache activity is on and its production has not ended. It returns the same context each time until production ends (THREAD Session and Failures), and from then on it returns null for good. SPEC-integrator.md sections 4.2 and 4.4 say what ends production, and its section 3.2, step 5, why a debugger attached before `start` leaves no production to begin with.
- R-INT-2. `ProducerBudget`, thread-safe and reference-counted: `[[nodiscard]] bool tryCharge(size_t)` and `void release(size_t)`, both callable from any thread, with THREAD's semantics for a charge past the limit (the charge fails, every later charge fails, and the VM thread raises the recording fault then or at its next capture or `delta`). A record may outlive its VM's session, so the budget object must outlive every record holding a reference to it. In twins builds, a budget without a limit that never raises a fault, for the twin compiles of section 11.3.
- R-INT-3. Strictness passed to `parseImageSections`, `captureImage` and `prepareImage` as the session's `strict`, and `validateImageSectionsAgainst` called only under strict.
- R-INT-4. `JSC::JITCache::didFailExecutableAllocation` (THREAD Execution), with the integrator's signature `void didFailExecutableAllocation(VM&, ExecutableAllocationSite)` and the contract SPEC-integrator.md section 4.5 declares for every caller; this lane passes `ExecutableAllocationSite::MathICSnippet`. This lane calls it on the VM thread from `JITMathIC::generateOutOfLine`, through `MathICRegeneration::didFailToAllocate`, inside a JIT operation that holds the API lock and no `CodeBlock::m_lock` (the repatching operations, such as `operationValueAddProfiledOptimize`, take no lock around `generateOutOfLine`).
- R-INT-5. Section type ids for `image.baseline`, `baked-facts.baseline` and, in twins builds, `image-twins.baseline`; each section handed to this lane as a span that starts 8-byte aligned and covers exactly the section.
- R-INT-6. Capture glue: `captureImage` only while the VM's production is active, on the VM thread with the API lock and heap access held, JS paused and no collector phase, on a `BaselineJITCode` it holds a reference to; the write functions called before JS resumes; `CaptureOutcome::RecordingFault` raised as a recording fault naming the check; `ChargeRefused` ends the capture without a second fault, since the budget raised it (R-INT-2); `NotEligible` treated as an ineligible CB.
- R-INT-7. Install glue: THREAD Restoration's order, which SPEC-integrator.md section 7.2 spells out; `compareBakedFacts` on the newborn CB before any lane writes it; `Mismatch` leaves the CB native and keeps the import; `prepareImage` before the other lanes' preparations, with `PreparedImage::code()` passed as the prepared image to the ICs lane's `prepareBaselineICs`; `PrepareOutcome::InvalidMaterial` turns cache activity off as invalid material; `ExecutableMemoryExhausted` raises the executable-allocation fault for JITCache's own image (THREAD Failures), `didFailExecutableAllocation(vm, ExecutableAllocationSite::JITCacheImage)`; a non-null budget exactly in ConsumerProducer VMs whose production is active, and null otherwise; `commit` called once, after every lane's preparation succeeded, and its result passed to `CodeBlock::setupWithUnlinkedBaselineCode`; a failed preparation of another lane destroys the `PreparedImage` without calling `commit`.
- R-INT-8. The header's build ID covers the object that contains `JITCache::codeSymbolAnchor` (THREAD Storage, the engine object when it is separate).
- R-INT-9. The rows of options.md that name this lane, in `start`'s table of fixed options.
- R-INT-10. The manifest entries of section 14.3.
- R-INT-11. Test builds (`ENABLE(JITCACHE_TWINS)`) and their runners. History: [Moving the domains](SPEC-image-history.md#moving-the-domains).
  - Twins builds compile and link with Bun's build flags like every other build, so none is position-independent, and the relocation clause skips the targets those flags pin (THREAD Verification, section 11.4).
  - A per-VM `JITCache::Twins` object, created no later than the VM's first `Twins::checkImage` and destroyed during VM destruction, at any point, since it holds no twin CB (section 11.3, step 2).
  - The `image-twins.baseline` span passed to `parseImageSections` in `ImageSectionSpans`, and the payload kept alive until `Twins::checkImage` returns.
  - A call to `Twins::checkImage` for every installed import, at the end of `ScriptExecutable::prepareForExecutionImpl` after `installCode` and before JS runs, on both install points (before `setupLLInt`, and in `JIT::compileSync` reached through `setupJIT`), with the installed CB, the scope `prepareForExecutionImpl` received, the committed `BaselineJITCode` and the parsed sections.
  - The producer-then-consumer runner. It runs every process with strict on (THREAD Verification) and turns `useConcurrentJIT` off in every process of a twins run unless the run's options turn it on, as T14's do; a skipped image check fails a run as harness sub-SPEC section 7.4 applies THREAD Verification's skip rule (section 11.3).
  - The runner moves every relocation domain the clause of section 11.4 compares between each process that imports a body and each process whose captures it imports; a ConsumerProducer that recaptures a body is that body's capturing process for every process that imports it later (T15). The importing process's executable pool lies in a range disjoint from the capturing process's (`g_jscConfig.startExecutableMemory` to `endExecutableMemory`), and its structure reservation at another base (`startOfStructureHeap`); both are settled before its `JSC::initialize` makes them (N22), for example with an inaccessible placeholder mapping over every free page of the capturing process's two ranges, or, for the pool, with `jitMemoryReservationAddress`, a free option. Address randomization stays on in every process (`kernel.randomize_va_space` at 2, no `ADDR_NO_RANDOMIZE` personality). The runner checks on each architecture that the heap moved, and where the build's allocator keeps a fixed base, it takes the heap from an allocator whose mappings the kernel places.
  - A run whose only twin reports are equal relocation pairs in the heap domain is repeated once in fresh processes, and an equal pair in the repetition fails it. A heap coincidence is chance, which fresh processes do not repeat, while a defect, such as a resolution that ignores its context, recurs.
  - A twins-only run flag that sets the process's image test hook (section 16.1) before its first VM: `--jitcache-test-image-hook=<name>`, a flag of the jsc shell only (harness sub-SPEC section 5.1, SPEC-integrator.md section 11.2), with `<name>` one of `relocation-pairs`, `operation-pair`, `change-recorded-target` and `skip-patch`, calling `JITCache::setImageTestHook`.
  - The C++ unit-test runner for `Source/JavaScriptCore/jitcache/tests/`, and a `TwinReport` sink that keeps a skipped check, with its reason, apart from a difference.
- R-INT-12. Whether a plan's compilation records (section 4.1): the integrator's edit to `BaselineJITPlan::BaselineJITPlan(CodeBlock*)` computes, on the VM thread, whether `producerContext(vm)` returns a context and the UCB registry's `keyOf(*codeBlock->unlinkedCodeBlock())` (SPEC-ucb.md section 6.1) returns a key. It stores the result in the plan, which exposes it as `bool BaselineJITPlan::jitCacheRecordsImage() const`. The member is written once, before the plan is enqueued or compiled, and only read afterward, from whichever thread compiles, so it needs no lock. A VM with no JITCache state pays one null test. A plan the image check constructs for its twin (section 11.3, step 4) computes the flag the same way, and the twin recorder that `setJITCacheTwin` installs does not read it.

## 13. Failures

| step | failure | outcome |
|---|---|---|
| recording, any thread | a charge refused | record `Incomplete`; emission continues and links every branch (section 4.8); the budget raises the recording fault (R-INT-2) |
| recording | a reason of section 4.1 | record `Unrecordable`; emission continues as after a refused charge; no fault; the body is not captured from this code |
| `JIT::link` | no executable memory | native: `CompilationFailed`; the recorder is dropped and releases its charges; the integrator raises the executable-allocation fault (SPEC-integrator.md section 9) |
| MathIC regeneration | a refused charge | record `Incomplete`, recording fault through the budget |
| MathIC regeneration, any tier, any configured VM | snippet allocation fails | `didFailExecutableAllocation` through `MathICRegeneration::didFailToAllocate`, raised before the native fallback continues (section 6.2) |
| capture | record not `Complete` (shareable code only, section 4.7), unknown mold identifier, MathIC provenance mismatch | `NotEligible` |
| capture | strict S1 to S4 | `RecordingFault` |
| capture | charge refused | `ChargeRefused`: the capture stops; the budget raises the recording fault (R-INT-2) |
| write | sink refuses | write returns false; the writer's storage failure (recording fault, integrator) |
| parse and validate, under strict | any V, U or W check, a footprint encoding included | invalid material |
| install | baked facts differ | `Mismatch`: CB native, import kept |
| prepare | strict S5 | invalid material |
| prepare | image or snippet allocation fails | `ExecutableMemoryExhausted`: the executable-allocation fault |
| prepare | rebuilt record refused | import proceeds without a record; the budget raises the recording fault (R-INT-2) |
| support generation | thunk allocation fails (must-succeed) | native crash, as at the VM's first use of that thunk |

A failure inside `prepareImage` frees the image, the snippets, the MathICs and the tables (I11); thunks generated during resolution remain, as if native code had asked for them.

## 14. Native edits and owned paths

### 14.1 Edits

No other part edits these functions (M8 covers the one class another lane also lists), and none is in a hot file. "Compile thread" means a JIT worklist thread inside its `Safepoint`, or the VM thread for a synchronous compilation, holding no JSC lock; "VM thread" means the thread holding the API lock and heap access.

| file | function or member | edit | context |
|---|---|---|---|
| assembler/AbstractMacroAssembler.h | namespace scope | the declaration block of section 4.3: `class JITCache::ImageRecorder;` and `JITCache::noteSupportLink(ImageRecorder&)` | any thread |
| assembler/AbstractMacroAssembler.h | `AbstractMacroAssemblerBase` | `JITCache::ImageRecorder* m_jitCacheRecorder`, `jitCacheRecorder()`, `setJITCacheRecorder()`, beside `m_randomSource` | any thread |
| assembler/AbstractMacroAssembler.h | `DataLabelPtr::label()`, `Jump::assemblerLabel()`, `Jump::isConditionalForJITCache()` (ARM64: type other than `JumpNoCondition` and `JumpNoConditionFixedSize`; x86_64: false) | new accessors | any thread |
| assembler/AbstractMacroAssembler.h | `Call::linkThunk`, `Jump::linkThunk` | when `masm->jitCacheRecorder()` is non-null, call `JITCache::noteSupportLink(*recorder)` (section 4.4) | compile thread, VM thread |
| assembler/AbstractMacroAssembler.cpp, .h | `AbstractMacroAssemblerBase::initializeRandom`; new `seedRandomForTwins(uint32_t)` | twins builds: `initializeRandom` reports the seed it draws to the attached recorder (`didInitializeRandom`); `seedRandomForTwins` emplaces `m_randomSource` with a given seed and `ASSERT`s that nothing was drawn yet | compile thread, VM thread |
| assembler/LinkBuffer.h | new `LinkBuffer::offsetOf(AssemblerLabel)` | `applyOffset(label).offset()` | compile thread, VM thread |
| assembler/MacroAssemblerARM64.h | new `MacroAssemblerARM64::memoryTempRegisterForReference()` | returns `getCachedMemoryTempRegisterIDAndInvalidate()` | compile thread, VM thread |
| jit/BaselineJITCode.h, .cpp | `BaselineJITCode` | `std::unique_ptr<JITCache::ImageRecord> m_jitCacheImageRecord`, with `class JITCache::ImageRecord;` forward-declared; the constructor and destructor, already out of line in `BaselineJITCode.cpp`, see the complete type there | written on the compile thread in `JIT::link`, then VM thread only |
| jit/JITCodeMap.h | `JITCodeMap` | new public `unsigned size() const` and `template<typename Functor> void forEach(const Functor&) const`, which calls the functor with each entry's `BytecodeIndex` and `CodeLocationLabel<JSEntryPtrTag>` in increasing bytecode index, from the private `indexes()` and `codeLocations()` (N27); capture reads the code map through it (section 9, step 2) | VM thread |
| jit/BaselineJITCode.h | new `template<typename Functor> void MathICHolder::forEachMathIC(const Functor&) const` | calls the functor with each `JITAddIC&`, `JITMulIC&`, `JITSubIC&` and `JITNegIC&` the holder owns, so the IC's type gives its kind (S3) | VM thread |
| jit/JIT.h, JIT.cpp | `JIT` | `std::unique_ptr<JITCache::ImageRecorder> m_imageRecorder` (the constructor and destructor are out of line in `JIT.cpp`); new `static ThunkGenerator baselineThunkGenerator(JITCache::BaselineThunk)`, defined in `JITPropertyAccess.cpp` (section 3.4), with `enum class JITCache::BaselineThunk : uint8_t;` declared opaquely; new `template<typename Op> static auto mathICGeneratorFor(const UnlinkedCodeBlock&, const JSInstruction*)`, defined in `JITInlines.h`, which JSC does not export and both callers include, since a deduced return type needs the definition at the call; twins builds: new `setJITCacheTwin(Ref<JITCache::ProducerBudget>&&, const JITCache::TwinSeeds&, const JITCache::TwinCompileInputs&)` (section 11.3, step 4), its three types forward-declared, which creates the twin recorder in `m_imageRecorder` at once, so `JIT` gains no member that exists only in twins builds (section 14.4) | compile thread |
| jit/JIT.cpp | `JIT::compileAndLinkWithoutFinalizing` | create the recorder when section 4.1's conditions hold, `m_plan.jitCacheRecordsImage()` (R-INT-12) among them, unless `setJITCacheTwin` already created a twin recorder, and attach it with `attachTo`; record capability and taint; in twins builds, hand the recorder `JITCache::snapshotCompileInputs(*m_profiledCodeBlock)` before the main pass (section 11.1); emit veneers before the `LinkBuffer` | compile thread |
| jit/JIT.cpp | `JIT::link` | `finishBaselineCompile` (section 4.7) | compile thread |
| jit/JIT.cpp, JITInlines.h, JITOpcodes.cpp, JITPropertyAccess.cpp, JITCall.cpp, JITArithmetic.cpp | the emitters the census names, except census row E2's `JIT::compileOpCall` and `JIT::compileTailCall`, which take no edit here (census E2) | the census's helpers, baked facts, `noteMathIC`, `markUnrecordable` | compile thread |
| jit/JITOpcodes.cpp | `JIT::compileOpStrictEq`, `JIT::compileOpStrictEqJump` | twins builds: when a recorder is attached, the operand the `tryGetAtomStringConstant` lambdas chose (N25) passes through `ImageRecorder::strictEqualityAtomOperand`, and the template emits the fast path for the operand that call returns, or the generic comparison for none (section 11.1) | compile thread |
| jit/JITArithmetic.cpp | `JIT::emitMathICFast` (both) | use `mathICGeneratorFor` | compile thread |
| jit/SlowPathCall.cpp | `JITSlowPathCall::call` | `nearCallSupport(SlowPathThunk)` | compile thread |
| jit/AssemblyHelpers.cpp | `emitExceptionCheck`, `emitNonNullDecodeZeroExtendedStructureID`, `branchIfValue`, `getArityPadding`, `restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(EntryFrame*&)`, `copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer`, `emitVirtualCall`, `emitVirtualCallWithoutMovingGlobalObject` | census helpers when a recorder is attached | any thread |
| jit/AssemblyHelpers.h | namespace scope; the inline `prepareCallOperation`, `barrierBranch(VM&, GPRReg, GPRReg, bool)`, `barrierBranch(VM&, JSCell*, GPRReg)`, `barrierBranchWithoutFence(JSCell*)`, `jumpIfMutatorFenceNotNeeded` and `copyCalleeSavesToEntryFrameCalleeSavesBuffer(EntryFrame*&, GPRReg)` | the declaration block of section 4.3; each inline function calls its hook when the assembler has a recorder (census A2, A4, A5, A10, A18) | any thread |
| jit/CCallHelpers.h | namespace scope; `CCallHelpers::jumpToExceptionHandler`; the `TrustedImm` overload of `setupArgumentsImpl` | the declaration of `JITCache::notePointerArgument`; when the assembler has a recorder, `jumpToExceptionHandler` calls `loadPtrFromVMAddress` (A12) and the overload calls `notePointerArgument` for a nonzero `TrustedImmPtr` (section 4.4) | any thread |
| jit/BinarySwitch.h, .cpp | `BinarySwitch` | `setRankedComparisons(BinarySwitchRankedComparisons*)`, used by `advance` for `IntPtr` comparisons; `caseRank()`; twins builds: a constructor overload `BinarySwitch(GPRReg, std::span<const int64_t>, Type, JITCache::ImageRecorder*)` that seeds `m_weakRandom` with `recorder ? recorder->binarySwitchSeed(globalCounter++) : globalCounter++`, since the constructor draws (N19); the JIT's switch emitters pass `m_imageRecorder.get()` | compile thread |
| jit/JITMathIC.h, new jit/JITMathIC.cpp | `JITMathIC::generateOutOfLine` | the header keeps the two declarations and forward-declares `JITCache::MathICRegeneration`; the new `.cpp` holds the wrapper, which passes `callReplacement` and the profile's bits to the `MathICRegeneration`, the native body moved from the header with the edits of section 6.2, and the explicit instantiations for the four ICs | VM thread, JIT operation (twin check for a twin replay) |
| jit/JITOperations.h, .cpp | `operationArithNegateProfiledOptimize`; new `operationArithNegateProfiledNoOptimize`; file-static `profiledArithNegate` | section 6.4 | VM thread, JIT operation |
| bytecode/ArithProfile.cpp | `ArithProfile::emitUnconditionalSet` (both overloads), and nothing else in the class | `or16AtReference` with `recorder->arithProfileTarget(this)` | any thread; see M8 |
| bytecode/CallLinkInfo.cpp | `CallLinkInfo::emitFastPathImpl` | `moveReference(ProcessThunk::DefaultCall)` for the default call target | compile thread |
| bytecode/CodeBlock.cpp | `CodeBlock::~CodeBlock` | twins builds: call `JITCache::forgetImageTwin(*this)` before anything else, and skip the `didOptimize` write when it returns true (section 11.3, step 2) | the thread that sweeps the VM's heap, `Heap::lastChanceToFinalize` included |

`BinarySwitchRankedComparisons` is an abstract class in `BinarySwitch.h` with one virtual function, `MacroAssembler::Jump branch(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg value, unsigned rank, intptr_t key)`, which section 4.3's `StringSwitchRecording` implements.

### 14.2 Owned paths

New files, all in this lane:

- `Source/JavaScriptCore/jitcache/ImageTypes.h`: the types and enumerations of sections 3 and 5, `ImageCheck` and the declaration of `description(ImageCheck)`, which `ImageSection.cpp` defines.
- `Source/JavaScriptCore/jitcache/ImageSupport.h`, `.cpp`: `CodeSymbol`, `codeSymbolAnchor`, `resolveTarget`, `resolveSupport`, the tables of section 3.6.
- `Source/JavaScriptCore/jitcache/ImageEmission.h`, `.cpp`: section 4.3, `StringSwitchRecording`, and the definitions of the hooks of section 4.3 (`noteSupportLink` among them), which the exported headers declare.
- `Source/JavaScriptCore/jitcache/ImageRecorder.h`, `.cpp`: section 4.
- `Source/JavaScriptCore/jitcache/ImageRecord.h`, `.cpp`: section 5 and `MathICRegeneration`.
- `Source/JavaScriptCore/jitcache/BakedFacts.h`, `.cpp`: section 7 and its section codec.
- `Source/JavaScriptCore/jitcache/ImageSection.h`, `.cpp`: section 8, the readers, writers, canonicalization and validation.
- `Source/JavaScriptCore/jitcache/ImageCapture.h`, `.cpp`: section 9.
- `Source/JavaScriptCore/jitcache/ImagePrepare.h`, `.cpp`: section 10.
- `Source/JavaScriptCore/jitcache/ImageTwins.h`, `.cpp`: section 11, the `image-twins.baseline` codec and checks of section 11.2 included, compiled only with `ENABLE(JITCACHE_TWINS)`.
- `Source/JavaScriptCore/jitcache/tests/ImageSectionTests.cpp`, `ImageRecordingTests.cpp`: section 16.
- `JSTests/jitcache/image/` and `JSTests/stress/negate-mathic-profile-after-regeneration.js`: section 16.
- `Source/JavaScriptCore/jit/JITMathIC.cpp`: the definitions of `JITMathIC::generateOutOfLine` and their explicit instantiations (section 6.2).

File-local names follow SPEC-integrator.md R-ALL-8, with the prefix `image`.

### 14.3 Manifest entries for `docs/JitCache/specs/INTEGRATE-image.md`

- M1. `Source/JavaScriptCore/Sources.txt`: add `jit/JITMathIC.cpp`, `jitcache/ImageSupport.cpp`, `jitcache/ImageEmission.cpp`, `jitcache/ImageRecorder.cpp`, `jitcache/ImageRecord.cpp`, `jitcache/BakedFacts.cpp`, `jitcache/ImageSection.cpp`, `jitcache/ImageCapture.cpp`, `jitcache/ImagePrepare.cpp` and `jitcache/ImageTwins.cpp` (empty without `ENABLE(JITCACHE_TWINS)`).
- M2. `Source/JavaScriptCore/CMakeLists.txt`: `jitcache` among the private include directories if the module's other SPECs do not already add it. No header of this lane is exported, and no exported header the lane edits includes one (section 14.4).
- M3. Section type ids (R-INT-5).
- M4. The fixed-option rows of options.md that name this lane (R-INT-9).
- M5. `didFailExecutableAllocation` (R-INT-4), `producerContext` and `ProducerBudget` (R-INT-1, R-INT-2).
- M6. Capture and install glue calls (R-INT-6, R-INT-7).
- M7. Test-build harness pieces (R-INT-11).
- M8. Conditional: the UCB lane lists class template `ArithProfile` among its edits (it adds `restoreBits` in `ArithProfile.h`). This lane changes only the two definitions of `ArithProfile::emitUnconditionalSet` in `ArithProfile.cpp`, which the UCB lane does not edit. If the integrator treats the class as one unit of ownership, it applies this lane's A14 change to those two functions from the manifest instead of task 3.

### 14.4 Exported headers

JSC copies the headers `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` lists (`Source/JavaScriptCore/CMakeLists.txt`) into `JavaScriptCore/PrivateHeaders`, and Bun compiles against those copies: `scripts/build/deps/webkit.ts` in `~/bun` gives Bun as include roots the WebKit build directory and the `Headers` and `PrivateHeaders` copies of JavaScriptCore, WTF and bmalloc inside it, and no directory of the source tree. Bun includes `JavaScriptCore/JIT.h` (`NodeVM.cpp`, `NodeVMScript.cpp`, `NodeVMSyntheticModule.cpp`, `BunJSCModule.h`), which reaches `BaselineJITCode.h`, `JITMathIC.h`, `LinkBuffer.h`, `CCallHelpers.h`, `AssemblyHelpers.h` and, through `MacroAssembler.h`, `AbstractMacroAssembler.h` and the architecture's macro assembler. A `jitcache/` header included from any of them would not resolve in Bun's build. History: [Exported headers](SPEC-image-history.md#exported-headers).

The exported headers this lane edits are `assembler/AbstractMacroAssembler.h`, `assembler/LinkBuffer.h`, `assembler/MacroAssemblerARM64.h`, `jit/AssemblyHelpers.h`, `jit/BaselineJITCode.h`, `jit/CCallHelpers.h`, `jit/JIT.h`, `jit/JITCodeMap.h`, `jit/JITMathIC.h` and `jit/JITOperations.h`. Its edits there add only:

- forward declarations of `JITCache` classes and structs, and opaque declarations of `JITCache` enumerations with their underlying type;
- the hook declarations of section 4.3, whose signatures use JSC types and references to forward-declared classes;
- members that hold a raw pointer to a forward-declared class (`AbstractMacroAssemblerBase::m_jitCacheRecorder`), or a `std::unique_ptr` to one in a class whose constructor and destructor are defined in a `.cpp` that sees the complete type (`BaselineJITCode`, `JIT`);
- members and declarations with JSC types only (`LinkBuffer::offsetOf`, `MacroAssemblerARM64::memoryTempRegisterForReference`, `MathICHolder::forEachMathIC`, `JITCodeMap::size`, `JITCodeMap::forEach`, `operationArithNegateProfiledNoOptimize`).

No data member of an exported class exists only under `ENABLE(JITCACHE_TWINS)`: a class keeps one layout in every translation unit, Bun's included, whichever switches a unit sees. Twins-only additions to exported classes are member functions (`AbstractMacroAssemblerBase::seedRandomForTwins`, `JIT::setJITCacheTwin`).

A body that needs more moves to a `.cpp`, as `JITMathIC::generateOutOfLine` does (section 6.2). Every `jitcache/` header this lane adds is included only by `.cpp` files and by headers JSC does not export; the unexported headers the lane edits (`JITInlines.h`, `BinarySwitch.h`) include them freely, since no exported header includes those. No lane exports a header, this one included.

## 15. Invariants

- I1. Under a recorder, two compilations of the same body with the same inputs and draws, in any two processes, produce images and snippets (section 3.1) of the same linked sizes whose bytes differ only inside fixup footprints.
- I2. A record's and a section's fixups, snippets' included, are in footprint order (section 3.2), each footprint inside its linked size.
- I3. Each fixup's form is one table 3.3 allows for its kind, and its fields meet that kind's condition in the table's strict-check column.
- I4. Without a recorder, every edited function emits the same bytes as the unedited engine.
- I5. A record is `Complete` only if every far call with a nonnull callee has an `Operation` fixup, every thunk link under the recorder went through a helper, no reason of section 4.1 occurred and every charge succeeded.
- I6. After `JIT::link` returns, the record is read and written only on the thread holding the VM's API lock; only its destructor runs elsewhere.
- I7. Each MathIC record with inline code has exactly one `Operation` fixup at `slowCallPointerSite`, naming the operation the slow call currently targets; a MathIC with `m_code` has provenance for exactly that allocation; a MathIC whose inline start was rewritten has exactly one `SnippetEntry` fixup there. A MathIC without inline code has null locations, no slow call, no snippet and no fixup that names it, in the producer and after import alike.
- I8. Inside every fixup footprint a captured section holds the canonical encoding; every other byte equals the live code, and no byte comes from past an allocation's linked size.
- I9. Capturing an imported image in a ConsumerProducer VM before any MathIC regenerates and before the coverage rates change reproduces the imported `image.baseline` and `baked-facts.baseline` byte for byte. In twins builds it also reproduces the imported `image-twins.baseline`, except its producer values, which hold the capturing VM's own resolutions, and its capture-process token, which is the capturing process's.
- I10. A prepared image's first `codeSize` bytes, and each prepared snippet's first `snippetSize` bytes, equal the section's outside fixup footprints, and each footprint decodes to the consumer's resolution of its target.
- I11. `prepareImage` writes nothing to the CB, the UCB or the VM's statistics, and frees everything it allocated when it fails; `commit` is the only step with outside effects.
- I12. An imported `BaselineJITCode` has the producer's calls, molds (order and fields), code map, constant pool and switch tables as offsets, the captured coverage rates, `m_isShareable` set and no `PCToCodeOriginMap`.
- I13. `compareBakedFacts` writes nothing to the CB.
- I14. A recorder, a record or an `ImageCapture` releases exactly the bytes it charged, on whatever thread destroys it.
- I15. For an inline string switch with `n` keys, the image has one `SwitchStringRankCase` fixup per rank in `[0, n)` and at least one `SwitchStringRankAtom` fixup per rank; the consumer writes the rank-`r` key of its own signed address order at every atom fixup of rank `r` and points the rank-`r` case jump at that key's case.
- I16. No baseline `negate` IC passes its own address to an operation that takes a `UnaryArithProfile*`.
- I17. A recorded image references no VM data outside table 3.6.
- I18. Every section that passes V1 to V7 and U1 to U7 (and in twins builds W1 to W4) can be prepared without an out-of-range access, with strict on or off, and every writer of section 10.3 writes only inside its fixup's footprint, apart from the jump islands the ARM64 writers allocate.
- I19. After a recorder stops recording, every branch its compilation or regeneration emits is still linked: an ARM64 conditional external jump either reaches a veneer that `emitVeneers` links or is linked natively at once.
- I20. The twin check changes nothing a run or a capture reads: it overwrites the UCB's arithmetic profiles only in a process running with `useConcurrentJIT` off, where no compiler thread reads them meanwhile, and restores them on every exit; the twin CB is never installed, keeps nothing alive past the check and dies without writing `didOptimize` (section 11.3, step 2), the twin's code is freed with the check, and no fault is raised.
- I21. The exported headers this lane edits (section 14.4) gain no include of a `jitcache/` header, and the lane exports no header. What any exported header may include is SPEC-integrator.md II18's rule.
- I22. In twins builds, a recording compilation records, at every strict-equality instruction whose template reaches its atom test, which operand it compared inline by its atom, if any (N25), and a twin compile makes the same choice there whatever atoms the consumer's UCB holds.

## 16. Tests and bench

### 16.1 Test obligations

Every test runs on x86_64 and ARM64 in the builds and at the cadence HARNESS.md gives, through the runners of R-INT-11, with strict on (THREAD Verification); the C++ tests pass `strict` explicitly.

The JS tests follow SPEC-integrator.md R-ALL-4 and the runner's directives and argument conventions (harness sub-SPEC sections 7.2 to 7.6).

Test hooks. Twins builds give the runner four hooks, one per process, set through the run flag of R-INT-11 before the process's first VM (`ImageTestHook`, section 11.3): `RelocationPairs` makes `checkImage` take, as the producer value of the first fixup it compares in each domain the body's fixups reach, that target's resolution in the restored context, so the relocation clause reports exactly those fixups (T3); `OperationPair` does the same for the body's first `Operation` fixup, which the clause skips, so it reports nothing (T3); `ChangeRecordedTarget` makes capture change the target of the record's first fixup before S1 runs (T7); `SkipPatch` makes `prepareImage` skip its first patch (T7). The guards of T11 are C++ hooks that the unit tests call directly.

- T1. Oracle. `JSTests/jitcache/image/` holds programs that, together, make the producer emit every target kind and form of table 3.3 and every census site: exceptions and `catch`, `switch` over dense and list integer and character tables, inline and large `switch_string`, `add`, `sub`, `mul` and `negate` through each regeneration path, a full inline snippet of a `CannotCompile` body (`noDFG`) through its first regeneration, and sites without inline code (a `+` that only saw strings, `"x" + y`, a `*` that only saw BigInts, `-` on a BigInt), strict equality against atom constants, regexp literals, getters and setters by id, `iterator_next` and async iteration, `for-in` enumeration, write barriers, `with` and `eval` for the scope kinds, module code, closures with scope depth below and above eight, `new Function`, direct eval, varargs, tail calls, arity fixup, stack overflow, `instanceof`, `length`, private names, and a `DEFINE_SLOW_OP` op followed at once by a `mov` of a string constant, which on ARM64 puts a `Call` and a `Pointer` at one site (section 3.2). Each runs as producer, then as consumer of the same artifact in a fresh process; the consumer's output, exceptions and stack traces equal a JITCache-off run. A program that reaches the stack limit prints only that the stack-overflow `RangeError` happened, never the depth it reached (THREAD Verification).
- T2. Twins. On the same programs, in twins builds driven by the runner of R-INT-11, every installed import passes the twin check of section 11.3, and none is skipped.
- T3. Relocation. On both architectures, no run of T2 fails the relocation clause of section 11.4, a run that R-INT-11 repeats being judged by its repetition. With the `RelocationPairs` hook the check reports exactly the fixups it forced, one in each domain the clause compares that the body's fixups reach, so a domain that did not move cannot pass unnoticed; with `OperationPair`, whose fixup's target lies in the engine image at its link-time address, it reports nothing. A ConsumerProducer run that imports bodies it committed itself, after their UCBs died and through a live attach, reports no relocation pair for them and passes every other comparison, while the hook still makes the same run report its forced fixups in its imports of the producer's bodies. Each run with the `RelocationPairs` hook declares a `jitcache-expect-twin` coincidence for each domain its forced fixups reach, so the runner requires those reports and fails on none of them (SPEC-integrator.harness.md section 7.2).
- T4. Round trip. A ConsumerProducer run that imports and captures again before any regeneration writes sections equal to the imported ones, the twins section's producer values and capture-process token excepted (I9).
- T5. Determinism. Two captures of one compilation are equal; after a regeneration they differ only in that MathIC's entry, its snippet, its inline-start and slow-call fixups, and the coverage rates. A `CannotCompile` body's full inline snippet keeps its profile-write fixups across the rewrite, unchanged.
- T6. Validation (`ImageSectionTests.cpp`). With strict on, for each of V1 to V7 and U1 to U7, and in twins builds W1 to W4, a valid section mutated to violate exactly that check is rejected with that `ImageCheck`, without an out-of-range access under ASan; truncation at every region boundary is rejected. The mutations include a NaN and an out-of-range coverage rate for V1, a capability level of `CapabilityLevelNotSet` and a taint of 2 for V7, an empty string table for U2 and a code map missing one instruction start for U5. With strict off, every valid section parses to the view strict gives it. V3's mutations cover, on each architecture, a nonzero variable field and a wrong opcode in each form, an x86_64 `Jump` whose opcode is neither `E9` nor `0F 80` to `0F 8F`, an ARM64 `Pointer` whose three words name different registers, an ARM64 `Jump` at site 0 holding a `nop`, and an ARM64 `Pointer` listed before the `Call` whose site it shares, while the same pair in footprint order passes.
- T7. Strict. In twins builds, the `ChangeRecordedTarget` hook makes capture fail S1 as a recording fault, and `SkipPatch` makes the import fail S5 as invalid material. Each hook runs in a `Producer; Consumer` sequence of its own, set in the run it acts on: in the first, the producer with `ChangeRecordedTarget` declares `jitcache-expect-fault: 0:0 image.s1` and commits nothing; in the second, the consumer with `SkipPatch` declares `jitcache-expect-fault: 1:1 image.s5`. Neither consumer installs a body, so the script declares `jitcache-expect-no-install: 1` for both.
- T8. Executable memory. With `useExecutableAllocationFuzz`, failing the image allocation, a snippet allocation at import, and a snippet allocation in a regeneration each produce the outcome of section 13, and the run's output still equals the oracle. A run whose fuzz point fails the image or an import's snippet declares `jitcache-require-fault` at `exec-alloc.jitcache-image` and, since the fault turns cache activity off at that import, `jitcache-expect-no-install`; a run whose fuzz point fails a regeneration's snippet declares `jitcache-require-fault` at `exec-alloc.mathic-snippet`. Each failure must therefore happen.
- T9. Baked facts (`ImageRecordingTests.cpp`, live VM). `compareBakedFacts` returns `Mismatch` for a fresh CB whose capability class (its executable set with `ScriptExecutable::setNeverOptimize` before linking), taint (a source provider with a tainted origin) or any scope fact (its metadata written through the instruction's `metadata(CodeBlock*)` accessor) differs from the section, `Match` otherwise, and leaves `capabilityLevelState()` at `CapabilityLevelNotSet`.
- T10. `negate`. `JSTests/stress/negate-mathic-profile-after-regeneration.js` drives a baseline `negate` site through regeneration from an empty profile with JITCache off; `ImageRecordingTests.cpp` checks on a live VM that afterwards the IC's `arithProfile()` is unchanged and the UCB profile records types first seen after the regeneration.
- T11. Unrecordable (`ImageRecordingTests.cpp`, live VM, each body compiled under a recorder and never captured). A body generated after `VM::enableControlFlowProfiler`, as Bun's coverage turns the profiler on (N29), carries `op_profile_control_flow` and gets `Unrecordable(NotShareable)`. A builtin-mode function made with `createBuiltinExecutable` that calls `@superSamplerBegin` and `@superSamplerEnd` gets `Unrecordable(SuperSamplerOpcode)`. The two guards of section 4.4, triggered by test hooks, leave the record `Unrecordable`.
- T12. Budget. A producer with a tiny limit gets `Incomplete` records and the recording fault, captures nothing, and after VM destruction its budget's balance is zero with LSan clean. A sweep of limits from one `kRecordChargeStep` upward, which moves the refusal through the main pass, the slow paths, `emitVeneers` and a MathIC snippet of a body that throws and regenerates, leaves code whose output equals the oracle at every limit, on ARM64 included, so every conditional external jump was linked (I19). Each producer with a limit runs first in its sequence, so the script declares `jitcache-expect-fault: 0 budget.limit`. The tiny limit's sequence comes first, and its consumer, which has nothing to install, declares `jitcache-expect-no-install: 0:1`.
- T13. ARM64 islands. A consumer whose support code lies beyond `bl` range of the image (forced through executable-pool placement in a test configuration) patches through islands; the twin check and S1 and S5 pass.
- T14. Concurrency. T1 with `useConcurrentJIT` on: recording on worklist threads, records attached before finalization. In twins builds the twin check skips every import of these runs (section 11.3), and the oracle comparison applies as in T1.
- T15. ConsumerProducer. A producer runs until some MathICs regenerate and calls `delta`; a ConsumerProducer imports that body, runs until other MathICs regenerate and calls `delta`; then a fresh consumer imports the new body. Twins and oracle pass, including the new snippets, so the twin replays both processes' regenerations from the original compilation's inputs.
- T16. String switches. Inline `switch_string` with 1 to 64 keys whose atoms are allocated in different orders in producer and consumer; twins and oracle pass.
- T17. Linked size. Bodies whose image or a snippet ends short of its handle's size (on ARM64, any body whose compaction leaves the linked size off a granule boundary; on x86_64, under the libpas JIT heap, any whose size class exceeds the linked size) capture only the linked bytes, and T2's byte comparison passes for them while the producer's, the consumer's and the twin's slack bytes differ.
- T18. Atom-ness (twins builds). Functions compare string constants longer than an inline string with `===` and `!==`, as values and as branch conditions, with the constant on the left at some sites and on the right at others. They run from the jsc shell's bytecode cache in producer and consumer, so their constants decode as plain strings. The producer compiles them while some constants are still plain and uses those constants as property keys before `delta`. One consumer uses other constants as property keys before its first call, and a second calls the functions in another order. Every import passes T2's twin check, the images keep the producer's generic comparisons where the consumer's constants are now atoms, and every recorded atom operand is an atom in the consumer. In `ImageSectionTests.cpp`, W4 rejects a kind-8 input that names a plain-string or non-string operand, one moved to an instruction with a `null` operand, and a section missing one kind-8 input. The UCB lane's `atom-constants.js` (SPEC-ucb.md, section 13.3) runs the Bun form of the same case with twins on.
- T19. Exported headers. Bun's `bun-debug` build, which compiles Bun against the copied headers of section 14.4 and no source directory, succeeds with every edit of section 14.1 applied. A check over the copied `PrivateHeaders` finds no include of a `jitcache/` header in the headers section 14.4 lists (I21), and every `jitcache/` header that any copied header includes is itself among the copies (SPEC-integrator.md II18).
- T20. Scope thunk keys (`ImageRecordingTests.cpp`, live VM). A `get_from_scope` site profiled `ClosureVarWithVarInjectionChecks` and one profiled `GlobalPropertyWithVarInjectionChecks` each record a `BaselineThunk::GetFromScopeGlobalVar` fixup under a recorder and, compiled without one, near-call the thunk `JIT::baselineThunkGenerator(GetFromScopeGlobalVar)` gives, as the unedited engine does (census D4, I4).
- T21. Twin lifetime (`ImageRecordingTests.cpp`, live VM, `useConcurrentJIT` off). After `checkImage` checks an import whose UCB's `didOptimize` is `Indeterminate`, `imageTwinCountForTesting()` is 1 until `collectNow(Sync, CollectionScope::Full)`, which sweeps synchronously, and 0 after it, and the UCB's `didOptimize` is still `Indeterminate`. A CB of the same body linked natively and left unreachable the same way sets it to `False` (N18).

### 16.2 Bench obligations

- B1. Installation bound: this lane's share of an import (parse, baked-facts comparison, `prepareImage`, `commit`) per body, beside the native compilation cost THREAD defines and the body's code size and fixup count. It runs with strict off (THREAD Verification), so no V, U or W check and no S5 read-back is in it. THREAD's bench sets the fraction and the smallest body the bound covers.
- B2. Recording: compile time with and without a recorder on the same bodies; provenance bytes per image byte and per fixup; peak recorder memory; the charged bytes of all live records per live body; and, in a ConsumerProducer, the rebuilt record's bytes per import, which the producer limit pays before the VM records anything itself. This measures what the producer limit must cover to stay out of normal runs (THREAD Execution). If live records dominate it, the first change to measure is a record that interns its distinct targets in a per-record table and keeps a small index per fixup, since most fixups repeat a few dozen targets. `kRecordChargeStep` (4 KiB) is tuned here.
- B3. Execution parity of recorded forms: recorded-form code against native-form code (JITCache off) on microbenchmarks dense in exception checks, VM field accesses, structure decodes and inline string switches, on both architectures.
- B4. Capture pause: `captureImage` plus the writes, per KiB of image, per body.
- B5. Installation memory: section bytes per image byte, `prepareImage`'s transient memory and, in a ConsumerProducer, the bytes of the record step 14 of section 10.3 rebuilds.

## 17. Tasks

Ordered; each fits one implementation agent. A task that needs an integrator piece builds against the requirement's working signature.

1. `ImageTypes`, `ImageSupport` (with `JIT::baselineThunkGenerator`, declared in `JIT.h` and defined in `JITPropertyAccess.cpp`), `BakedFacts` and `ImageSection`: types, tables, `CodeSymbol`, the codecs, canonicalization, and the strict checks V1 to V7 (with V3's footprint encodings) and U1 to U7, which debug builds `ASSERT` when strict is off (section 8.5); T6 for those checks. No behavior change.
2. Assembler edits of section 14.1 (recorder pointer, label accessors, guards, `LinkBuffer::offsetOf`, the ARM64 accessor, the twins seed hooks), `ImageRecorder` (labels, veneer groups, charging, and the behavior of section 4.8 once recording stops) and `ImageEmission` with the hooks of section 4.3, all but `StringSwitchRecording`, which task 3 adds beside the `BinarySwitchRankedComparisons` class it implements, and `moveReferenceValue`, which task 3 adds with its I4 check; `ImageRecordingTests.cpp` checks I4 for every helper and hook, the fixed footprints with a recorder, and I19 with a budget that refuses midway.
3. The shared helpers of section 14.1 (`AssemblyHelpers`, `CCallHelpers`, `ArithProfile`, `CallLinkInfo`, `BinarySwitch` with `BinarySwitchRankedComparisons`, `JITSlowPathCall`), per part A of the census, the inline ones of the exported headers through the hooks (section 14.4), and `StringSwitchRecording` and `moveReferenceValue` in `ImageEmission`, with `moveReferenceValue`'s I4 check in `ImageRecordingTests.cpp`. After task 2.
4. `JIT` integration: recorder lifetime, veneers, `finishBaselineCompile` (the linked size, far calls, the footprint order of section 3.2, MathIC entries with and without inline code), the reasons of section 4.1, the `BaselineJITCode` field, `ImageRecord`, baked-facts recording. After tasks 1 to 3.
5. Census parts B and C (`JIT.cpp`, `JITInlines.h`, `JITOpcodes.cpp`). After task 4.
6. Census parts D, E and F (`JITPropertyAccess.cpp`, `JITCall.cpp`, `JITArithmetic.cpp`) and `mathICGeneratorFor`, with T20. After task 4.
7. The `negate` operations and T10's stress test. Independent; may land first. T10's live-VM check in `ImageRecordingTests.cpp` lands after task 2, which creates that file.
8. MathIC regeneration (the move of `JITMathIC::generateOutOfLine` to `jit/JITMathIC.cpp` with its explicit instantiations, the wrapper and body edits, the native `MathICRegeneration` with the snippet's linked size in its provenance). After tasks 4 and 6.
9. `ImageCapture` with S1 to S4, the `JITCodeMap` accessors of section 14.1 that its step 2 reads the code map through (N27), and `MathICHolder::forEachMathIC`, which S3 uses. After tasks 5, 6 and 8.
10. `ImagePrepare` with `PreparedImage::code()`, `compareBakedFacts` and S5; T9. After tasks 1, 4 and 6 (section 10.3, step 2, calls task 6's `JIT::mathICGeneratorFor`).
11. Twins, all under `ENABLE(JITCACHE_TWINS)`, after tasks 9 and 10. The producer side: `snapshotCompileInputs` with its API-lock flag and its call in `JIT::compileAndLinkWithoutFinalizing`, `ImageRecorder::recordCompileInputs`, the strict-equality inputs (`ImageRecorder::strictEqualityAtomOperand`, its calls in `JIT::compileOpStrictEq` and `JIT::compileOpStrictEqJump`, and the merge in `finishBaselineCompile`), the regeneration log in the native `MathICRegeneration` (section 11.1: the entry at construction, a slot per `attach`), and capture's producer values (section 9, step 6). The section: the `image-twins.baseline` codec with W1 to W4, `ImageSectionSpans::twins`, and the twin data of the rebuilt record (section 10.3, step 14). The check: `JIT::setJITCacheTwin`, the `BinarySwitch` seed overload, the twin recorder's strict-equality answers, the twin `MathICRegeneration`, `Twins::checkImage` with its preconditions and its relocation clause, including the skip of a target at its link-time address, `captureProcessToken` with the clause's rule for a body its own process captured, the twin registry (`rememberImageTwin`, `forgetImageTwin`, `imageTwinCountForTesting`) with its call in `CodeBlock::~CodeBlock`, the test hooks of section 16.1 with `setImageTestHook`, and T21.
12. The JS corpus and T1 to T5, T7, T8 and T11 to T19, once the integrator's glue and runners land.
13. B1 to B5.
