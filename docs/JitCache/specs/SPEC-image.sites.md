# SPEC-image.sites: the reference census

Binding sub-SPEC of [SPEC-image.md](SPEC-image.md). It lists every place, in the functions the Image lane edits, where code emitted into a baseline image or a MathIC snippet embeds a reference, and the helper of SPEC-image section 4.3 that site uses. SPEC-image section 4.6 states the rules C1 to C5 this census applies, cited here as "rule Cn" to keep them apart from rows C1 to C27; target kinds are those of its table 3.3, and VM data names those of its table 3.6. [SPEC-image-history.md](SPEC-image-history.md) records why the decisions it names are what they are; nothing in it binds.

Each row keeps the native expression so the site can be found with grep. "Recorder" means the assembler's `jitCacheRecorder()` is non-null. Sites inside headers JSC exports (A2, A4, A5, the inline function of A10, A12, A13 and A18) reach recording only through the hooks their headers declare (SPEC-image sections 4.3 and 14.4), and a hook emits the recorded sequence of the helper the row names.

## A. Shared helpers

Edited once, at the helper (rule C2).

| id | file and function | native expression | target | helper |
|---|---|---|---|---|
| A1 | jit/SlowPathCall.cpp `JITSlowPathCall::call` | `nearCallThunk(vm.jitStubs->ctiSlowPathFunctionStub(vm, m_slowPathFunction))` | `SlowPathThunk`, `CodeSymbol::of(m_slowPathFunction)` | `nearCallSupport` |
| A2 | jit/AssemblyHelpers.h `AssemblyHelpers::prepareCallOperation` (`ASSERT_ENABLED` builds) | `storePtr(callFrameRegister, &vm.topCallFrame)` | `VMAddress::TopCallFrame` | hook `storePtrToVMAddress`: `storePtrAtReference` |
| A3 | jit/AssemblyHelpers.cpp `AssemblyHelpers::emitExceptionCheck` | `branchTestPtr(cond, AbsoluteAddress(vm.addressOfException()))`; in `ASSERT_ENABLED` builds `branchPtr(Equal, AbsoluteAddress(vm.addressOfException()), exceptionReg)` | `VMAddress::Exception` | `branchTestPtrAtReference`; `branchPtrAtReference` |
| A4 | jit/AssemblyHelpers.h `AssemblyHelpers::barrierBranch(VM&, GPRReg, GPRReg, bool)` | `branch32(cond, scratchGPR, AbsoluteAddress(vm.heap.addressOfBarrierThreshold()))` | `VMAddress::BarrierThreshold` | hook `branch32WithVMAddress`: `branch32WithReferenceAt` |
| A5 | jit/AssemblyHelpers.h `AssemblyHelpers::jumpIfMutatorFenceNotNeeded` (reached through `mutatorFence`, which returns early on x86_64) | `branchTest8(Zero, AbsoluteAddress(vm.heap.addressOfMutatorShouldBeFenced()))` | `VMAddress::MutatorShouldBeFenced` | hook `branchTest8AtVMAddress`: `branchTest8AtReference` |
| A6 | jit/AssemblyHelpers.cpp `AssemblyHelpers::emitNonNullDecodeZeroExtendedStructureID` (reached through both `emitLoadStructure` overloads and `emitLoadPrototype`) | `or64(TrustedImm64(structureIDBase()), source, dest)` | `StructureIDBase` | `orStructureIDBase` |
| A7 | jit/AssemblyHelpers.cpp `AssemblyHelpers::branchIfValue` | `branchPtr(invert ? Equal : NotEqual, value, TrustedImmPtr(jsEmptyString(vm)))` | `VMCell::EmptyString` | `branchPtrWithReference` |
| A7a | same, the `JSGlobalObject*` alternative of `globalObject` | `move(TrustedImmPtr(globalObject), ...)` | none: baseline passes `LazyBaselineGlobalObject` | under a recorder, `markUnrecordable(UnannotatedReference)` and native |
| A8 | jit/AssemblyHelpers.cpp `AssemblyHelpers::getArityPadding` | `branchPtr(GreaterThan, AbsoluteAddress(vm.addressOfSoftStackLimit()), scratchGPR1)` | `VMAddress::SoftStackLimit` | `branchPtrAtReference` |
| A9 | jit/AssemblyHelpers.cpp `AssemblyHelpers::restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(EntryFrame*&)` | `loadPtr(&topEntryFrame, scratch)` | `recorder->vmAddressTarget(&topEntryFrame)`, which is `VMAddress::TopEntryFrame` for every baseline caller | `loadPtrAtReference` |
| A10 | jit/AssemblyHelpers.cpp `AssemblyHelpers::copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer(EntryFrame*&, const RegisterSet&)`, and the inline `copyCalleeSavesToEntryFrameCalleeSavesBuffer(EntryFrame*&, GPRReg)` in jit/AssemblyHelpers.h | `loadPtr(&topEntryFrame, destBufferGPR)` | as A9 | `loadPtrAtReference`; in the inline function, hook `loadPtrFromVMAddress` |
| A11 | jit/AssemblyHelpers.cpp `AssemblyHelpers::emitVirtualCallWithoutMovingGlobalObject` | `nearCallThunk(vm.getCTIVirtualCall(callMode))` | `VirtualCallThunk`, `callMode` | `nearCallSupport` |
| A11a | jit/AssemblyHelpers.cpp `AssemblyHelpers::emitVirtualCall` | `move(TrustedImmPtr(info), GPRInfo::regT2)` | none: optimizing tiers only | under a recorder, `markUnrecordable(UnannotatedReference)` and native |
| A12 | jit/CCallHelpers.h `CCallHelpers::jumpToExceptionHandler` | `loadPtr(&vm.targetMachinePCForThrow, GPRInfo::regT1)` | `VMAddress::TargetMachinePCForThrow` | hook `loadPtrFromVMAddress`: `loadPtrAtReference` |
| A13 | jit/CCallHelpers.h `CCallHelpers::setupArgumentsImpl`, the `TrustedImm` overload | a nonzero `TrustedImmPtr` argument | none | the guard of SPEC-image section 4.4, through hook `notePointerArgument` |
| A14 | bytecode/ArithProfile.cpp `ArithProfile::emitUnconditionalSet(CCallHelpers&, BitfieldType)` and `(CCallHelpers&, GPRReg)` | `or16(mask, AbsoluteAddress(addressOfBits()))` | `recorder->arithProfileTarget(this)`: `UCBBinaryArithProfile` or `UCBUnaryArithProfile` by the UCB vector `this` lies in; `addressOfBits()` is `this` (SPEC-image N26). A profile in neither vector (nullopt) is a programming error: an `ASSERT` in debug builds, and in release builds `markUnrecordable(InconsistentRecord)` and the native `or16` | `or16AtReference` |
| A15 | bytecode/CallLinkInfo.cpp `CallLinkInfo::emitFastPathImpl` | `move(TrustedImmPtr(LLInt::defaultCall().code().taggedPtr()), callTargetGPR)` | `ProcessThunk::DefaultCall` | `moveReference` |
| A15a | same, `callLinkInfo` non-null | `move(TrustedImmPtr(callLinkInfo), callLinkInfoGPR)` | none: optimizing tiers only; baseline passes null | under a recorder, `markUnrecordable(UnannotatedReference)` and native |
| A16 | jit/BinarySwitch.cpp `BinarySwitch::advance`, `IntPtr` cases of `NotEqualToFallThrough`, `NotEqualToPush` and `LessThanToPush` | `branchPtr(cond, m_value, ImmPtr(case value))` | `SwitchStringRankAtom(table, rank)` through `BinarySwitchRankedComparisons` | `branchPtrWithReference` (in `StringSwitchRecording`) |
| A17 | jit/JIT.cpp `JIT::exceptionCheck(Jump)` | `jumpToHandler.linkThunk(getCTIStub(CommonJITThunkID::HandleException))` | `CommonThunk::HandleException` | `linkJumpToSupport` |
| A18 | jit/AssemblyHelpers.h `AssemblyHelpers::barrierBranch(VM&, JSCell*, GPRReg)` and `barrierBranchWithoutFence(JSCell*)` | a cell's address and the barrier threshold | none: no baseline caller | under a recorder, hook `noteUnannotatedReference` (`markUnrecordable(UnannotatedReference)`), then native |

A14 covers every arithmetic-profile write in images and snippets: `emitSetDouble`, `emitSetNonNumeric`, `emitSetHeapBigInt`, `emitSetBigInt32` and `emitObserveResult` call it, and so do `JITAddGenerator`, `JITSubGenerator`, `JITMulGenerator`, `JITNegGenerator`, `JITDivGenerator`, `JIT::emit_op_to_number` and `JIT::emit_op_to_numeric`.

A17 covers the exception check after every operation call (`JIT::appendCallWithExceptionCheck`, `JIT::exceptionCheck()`).

## B. jit/JIT.cpp and jit/JITInlines.h

| id | function | native expression | target | helper |
|---|---|---|---|---|
| B1 | `JIT::compileAndLinkWithoutFinalizing` | `branchPtr(GreaterThan, AbsoluteAddress(m_vm->addressOfSoftStackLimit()), regT1)` | `VMAddress::SoftStackLimit` | `branchPtrAtReference` |
| B2 | same | `nearCallThunk(CodeLocationLabel { LLInt::arityFixup() })` | `ProcessThunk::ArityFixup` | `nearCallSupport` |
| B3 | same | `jumpThunk(getCTIStub(CommonJITThunkID::ThrowStackOverflowAtPrologue))` | `CommonThunk::ThrowStackOverflowAtPrologue` | `jumpSupport` |
| B4 | same | `getArityPadding(*m_vm, ...)` | A8 | A8 |
| B5 | `JIT::emitConsistencyCheck` (`ASSERT_ENABLED`) | `nearTailCallThunk(getCTIStub(consistencyCheckGenerator))` | `BaselineThunk::ConsistencyCheck` | `nearTailCallSupport` |
| B6 | `JIT::emitGetVirtualRegister` (JITInlines.h) | `moveValue(m_unlinkedCodeBlock->getConstant(src), dst)` for a UCB-owned constant | a cell: `UCBConstantCell(src.toConstantIndex())`; anything else is a literal | `moveReference` for a cell, native otherwise |
| B7 | `JIT::appendCallWithExceptionCheck`, both overloads (JITInlines.h, `ASSERT_ENABLED`) | `branchPtr(Equal, AbsoluteAddress(vm().addressOfException()), reg)` | `VMAddress::Exception` | `branchPtrAtReference` |
| B8 | `JIT::updateTopCallFrame` (JITInlines.h) | `prepareCallOperation(*m_vm)` | A2 | A2 |
| B9 | `JIT::appendCall` | far call | `Operation` | recorded centrally in `JIT::link` (SPEC-image section 4.7) |

The consistency-check near calls of `JIT::privateCompileMainPass` link to `m_consistencyCheckLabel` inside the image and carry no fixup. `JIT::setSamplingFlag`, `clearSamplingFlag` and `emitCount` exist only in builds with `ENABLE(SAMPLING_FLAGS)` or `ENABLE(SAMPLING_COUNTERS)`, which nothing defines (`wtf/PlatformEnable.h`). The lane leaves them unedited, and `ImageRecorder.cpp` fails the build with `#error` when either switch is on, so no recorder meets them.

## C. jit/JITOpcodes.cpp

| id | function | native expression | target | helper |
|---|---|---|---|---|
| C1 | `JIT::emit_op_mov` | `storeValue(m_unlinkedCodeBlock->getConstant(src), addressFor(dst))` for a UCB-owned constant | a cell: `UCBConstantCell`; else literal | `storeReferenceValue` (kind: untrusted `storeValue`) for a cell, native otherwise |
| C2 | `JIT::emit_op_new_object` | `mutatorFence(*m_vm)` | A5 | A5 |
| C3 | `JIT::emitSlow_op_new_object` | `TrustedImmPtr(&vm())` argument | `VMAddress::VM` | `ImageReference::vmAddress` |
| C4 | `JIT::emit_op_jfalse`, `JIT::emit_op_jtrue` | `nearCallThunk(getCTIStub(valueIsFalseyGenerator))`, `valueIsTruthyGenerator` | `BaselineThunk::ValueIsFalsey`, `ValueIsTruthy` | `nearCallSupport` |
| C5 | `JIT::emit_op_throw` | `jumpThunk(getCTIStub(op_throw_handlerGenerator))` | `BaselineThunk::OpThrowHandler` | `jumpSupport` |
| C6 | `JIT::compileOpStrictEq` | `branchPtr(NotEqual, regT5, TrustedImmPtr(string->tryGetValueImpl()))` | `UCBConstantAtom` of the string operand | `branchPtrWithReference` |
| C7 | `JIT::compileOpStrictEqJump` | `branchPtr(Equal or NotEqual, regT2, TrustedImmPtr(string->tryGetValueImpl()))` | `UCBConstantAtom` | `branchPtrWithReference` |
| C8 | `JIT::emit_op_catch` | `restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(vm().topEntryFrame)` | A9 | A9 |
| C9 | same | `move(TrustedImmPtr(m_vm), regT3)` | `VMAddress::VM` | `moveReference` |
| C10 | same | `TrustedImmPtr(&vm())` arguments of `operationRetrieveAndClearExceptionIfCatchable` and `operationTryOSREnterAtCatchAndValueProfile` | `VMAddress::VM` | `ImageReference::vmAddress` |
| C11 | same | `jumpToExceptionHandler(vm())` | A12 | A12 |
| C12 | `JIT::emit_op_switch_imm`, `JIT::emit_op_switch_char`, dense tables | `move(TrustedImmPtr(linkedTable.m_ctiOffsets.mutableSpan().data()), regT2)` | `SwitchTableBase(tableIndex)` | `moveReference` with `ImageReference::switchTableBase` |
| C13 | `JIT::emit_op_switch_string`, inline tree | `BinarySwitch` over atom addresses; each leaf `addJump(jump(), caseTargets[caseIndex()])` | A16 for comparisons; each leaf jump `SwitchStringRankCase(tableIndex, caseRank())` | a `StringSwitchRecording` passed to the switch; the leaf jump recorded by `StringSwitchRecording::recordCase` before `addJump` |
| C14 | `JIT::emitCheckTraps` | `branchTest32(NonZero, AbsoluteAddress(m_vm->traps().trapBitsAddress()), TrustedImm32(VMTraps::AsyncEvents))` | `VMAddress::TrapBits` | `branchTest32AtReference` |
| C15 | `JIT::emitSlow_op_check_traps` | `nearCallThunk(getCTIStub(op_check_traps_handlerGenerator))` | `BaselineThunk::OpCheckTrapsHandler` | `nearCallSupport` |
| C16 | `JIT::emit_op_enter` | `store8(TrustedImm32(1), vm().addressOfMightBeExecutingTaintedCode())` | `VMAddress::MightBeExecutingTaintedCode` | `store8AtReference` |
| C17 | `JIT::emitSlow_op_enter` | `nearCallThunk(getCTIStub(op_enter_handlerGenerator))` | `BaselineThunk::OpEnterHandler` | `nearCallSupport` |
| C18 | `JIT::emitSlow_op_loop_hint` | `copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer(vm().topEntryFrame)` | A10 | A10 |
| C19 | same | `TrustedImmPtr(&vm())` argument of `operationOptimize` | `VMAddress::VM` | `ImageReference::vmAddress` |
| C20 | `JIT::emit_op_super_sampler_begin`, `JIT::emit_op_super_sampler_end` | `add32` / `sub32(TrustedImm32(1), AbsoluteAddress(&g_superSamplerCount))` | none | `markUnrecordable(SuperSamplerOpcode)`, then native |
| C21 | `JIT::emit_op_new_reg_exp` | `TrustedImmPtr(uncheckedDowncast<RegExp>(m_unlinkedCodeBlock->getConstant(regexp)))` argument | `UCBConstantCell(regexp.toConstantIndex())` | `ImageReference::ucbConstantCell` |
| C22 | `JIT::emit_op_create_this` | `mutatorFence(*m_vm)` | A5 | A5 |
| C23 | `JIT::emit_op_debug` | `TrustedImmPtr(&vm())` argument of `operationDebug` | none | none: Debugger-mode bytecode only (rule C3); native |
| C24 | `JIT::emit_op_profile_type`, `JIT::emit_op_profile_control_flow` | the type profiler's log and `TypeLocation`, `BasicBlockLocation` counters | none | none: each emitter clears `m_isShareable`, so `finishBaselineCompile` makes the record `Unrecordable(NotShareable)` (SPEC-image section 4.7); native |
| C25 | `JIT::emit_op_log_shadow_chicken_prologue`, `JIT::emit_op_log_shadow_chicken_tail` | `ensureShadowChickenPacket` | none | none: Debugger-mode bytecode only (rule C3); native |
| C26 | `JIT::emit_op_to_number`, `JIT::emit_op_to_numeric` | `arithProfile->emitUnconditionalSet(...)` | A14 | A14 |
| C27 | `JIT::emit_op_typeof_is_undefined`, `emit_op_has_structure_with_flags`, `emit_op_jeq_null`, `emit_op_jneq_null`, `emit_op_eq_null`, `emit_op_neq_null`, `emit_op_get_prototype_of` | `emitLoadStructure`, `emitLoadPrototype` | A6 | A6 |

C6 and C7 compare against the operand the templates' `tryGetAtomStringConstant` lambdas chose (SPEC-image N25), which twins builds also record as a compile input through `ImageRecorder::strictEqualityAtomOperand` (SPEC-image section 11.1).

C20's opcodes come only from the `@superSamplerBegin` and `@superSamplerEnd` intrinsics (`BytecodeIntrinsicNode::emit_intrinsic_superSamplerBegin`, `emit_intrinsic_superSamplerEnd`), which lex only where `Lexer` sets `m_parsingBuiltinFunction`: in builtin-mode parsing, or under `exposePrivateIdentifiers`, which options.md fixes off. No JSC or Bun builtin calls them; only test hooks that parse builtin-mode functions from given text (`$vm.createBuiltin`, the jsc shell's builtin helpers) produce them.

`emit_op_new_func` and its siblings add constant-pool entries, a side table, and embed no reference. `emit_op_create_lexical_environment` uses its constant only to choose the operation.

## D. jit/JITPropertyAccess.cpp

| id | function | native expression | target | helper |
|---|---|---|---|---|
| D1 | `JIT::emitSlow_op_get_by_id`, `emitSlow_op_get_by_id_direct`, `emitSlow_op_get_length`, `emitSlow_op_get_by_id_with_this`, `emitSlow_op_get_by_val_with_this`, `emitSlow_op_put_by_id`, `emitSlow_op_put_private_name`, `emitSlow_op_in_by_id`, `emitSlow_op_in_by_val`, `emitSlow_op_del_by_id`, `emitSlow_op_del_by_val`, `emitSlow_op_get_private_name`, `emitSlow_op_set_private_brand`, `emitSlow_op_check_private_brand`, `emitHasPrivateSlow`, `generateGetByValSlowCase`, `generatePutByValSlowCase` | `nearCallThunk(InlineCacheCompiler::generateSlowPathCode(vm(), gen.accessType()))` | `InlineCacheSlowPathThunk(gen.accessType())` | `nearCallSupport` |
| D2 | `JIT::emit_op_put_getter_by_id`, `emit_op_put_setter_by_id`, `emit_op_put_getter_setter_by_id` | `TrustedImmPtr(m_unlinkedCodeBlock->identifier(bytecode.m_property).impl())` argument | `UCBIdentifier(bytecode.m_property)` | `ImageReference::ucbIdentifier` |
| D3 | `JIT::emit_op_resolve_scope`, `JIT::emitSlow_op_resolve_scope` | `nearCallThunk(getCTIStub(generateOpResolveScopeThunk<T>))` | `BaselineThunk::ResolveScope*` for `T` | `nearCallSupport`; plus the baked facts of SPEC-image section 7 in the fast path |
| D4 | `JIT::emit_op_get_from_scope`, `JIT::emitSlow_op_get_from_scope` | `nearCallThunk(getCTIStub(generateOpGetFromScopeThunk<T>))` | the `BaselineThunk::GetFromScope*` of the `T` the native chain links (below) | `nearCallSupport`; plus the `ClosureVar` baked fact |
| D5 | `JIT::emit_op_put_to_scope` | write barriers (D7) | D7 | D7; plus the baked facts of SPEC-image section 7 |
| D6 | `JIT::emitSlow_op_put_to_scope` | `JITSlowPathCall` for `ModuleVar`; else `nearCallThunk(getCTIStub(slow_op_put_to_scopeGenerator))` | A1; `BaselineThunk::SlowOpPutToScope` | A1; `nearCallSupport`; plus the `ModuleVar` baked fact |
| D7 | `JIT::emitWriteBarrier(VirtualRegister, VirtualRegister, WriteBarrierMode)`, `emitWriteBarrier(GPRReg, WriteBarrierMode)`, `emitWriteBarrier(GPRReg)` | `barrierBranch(vm(), ...)`; `TrustedImmPtr(&vm())` argument of `operationWriteBarrierSlowPath` | A4; `VMAddress::VM` | A4; `ImageReference::vmAddress` |
| D8 | `JIT::emitWriteBarrier(JSCell*)` | `TrustedImmPtr(owner)` | none: no baseline caller | under a recorder, `markUnrecordable(UnannotatedReference)` and native |
| D9 | `JIT::emit_op_enumerator_next` | `storeTrustedValue(vm().smallStrings.sentinelString(), addressFor(propertyName))` | `VMCell::SmallStringsSentinel` | `storeReferenceValue` (kind: `storeTrustedValue`) |
| D10 | the enumerator emitters (`emit_op_get_property_enumerator`, `emit_op_enumerator_next`, `emit_op_enumerator_put_by_val`) | structure decodes | A6 | A6 |

D4's key follows the native selection as written, never the profiled type alone. In `JIT::emit_op_get_from_scope`'s default branch the chain mixes `if` and `else if`, so `GlobalVarWithVarInjectionChecks` and `GlobalLexicalVarWithVarInjectionChecks` key their own thunks and every other type that reaches the branch keys `GetFromScopeGlobalVar`, `ClosureVarWithVarInjectionChecks` and `GlobalPropertyWithVarInjectionChecks` included. That branch adds no slow case, so `JIT::emitSlow_op_get_from_scope` runs only for `GlobalProperty`, `GlobalVar` and `GlobalLexicalVar`, each keyed to its own thunk. No baseline code links `generateOpGetFromScopeThunk<ClosureVarWithVarInjectionChecks>`, and `BaselineThunk` has no entry for it: keying the call by the profiled type would call that thunk, which reads the closure variable with no type check, change native bytes against SPEC-image I4 and bake a shape SPEC-image section 7 records no fact for. SPEC-image T20 checks both types. History: [The scope thunk key](SPEC-image-history.md#the-scope-thunk-key).

## E. jit/JITCall.cpp

| id | function | native expression | target | helper |
|---|---|---|---|---|
| E1 | `JIT::emit_op_ret` | `jumpThunk(getCTIStub(CommonJITThunkID::ReturnFromBaseline))` | `CommonThunk::ReturnFromBaseline` | `jumpSupport` |
| E2 | `JIT::compileOpCall`, `JIT::compileTailCall` | `CallLinkInfo::emitFastPath`, `emitTailCallFastPath` | A15 | A15; no edit in these functions |
| E3 | `JIT::compileCallDirectEvalSlowCase` | `emitVirtualCallWithoutMovingGlobalObject(*m_vm, ..., CallMode::Regular)` | A11 | A11 |
| E4 | `JIT::emitSlowIteratorOpenGeneric`, `JIT::emitSlow_op_iterator_next`, `JIT::emitSlow_op_instanceof` | `nearCallThunk(InlineCacheCompiler::generateSlowPathCode(vm(), gen.accessType()))` | `InlineCacheSlowPathThunk` | `nearCallSupport` |
| E5 | `JIT::emitIteratorOpenGeneric` and the `iterator_next` emitters | `JITSlowPathCall` | A1 | A1 |
| E6 | `JIT::emit_op_iterator_next` | `branchIfTruthy(vm(), ...)` | A7 | A7 |
| E7 | `JIT::emit_op_async_iterator_next` | `TrustedImmPtr(&vm().syncResumeCallCache())` argument | `VMAddress::SyncResumeCallCache` | `ImageReference::vmAddress` |

E2's functions take no edit of this lane: their only reference is emitted inside A15's `CallLinkInfo::emitFastPathImpl`, in bytecode/CallLinkInfo.cpp. The `super_construct` block of `JIT::compileOpCall` belongs to the ICs lane (SPEC-ics.md E1), the only edit that function takes; its `JSCell::seenMultipleCalleeObjects()` is a literal (SPEC-image R-ALL-1). Immortal identifiers of the molds built here (`next`, `done`, `value`, `Symbol.hasInstance`, `prototype`) are side-table content, encoded by name (SPEC-image section 8.2).

## F. jit/JITArithmetic.cpp

| id | function | native expression | target | helper |
|---|---|---|---|---|
| F1 | `JIT::emit_op_add`, `emit_op_sub`, `emit_op_mul`, `emit_op_negate` | `m_mathICs.addJIT*IC(arithProfile)` | none | `noteMathIC` right after |
| F2 | `JIT::emitMathICFast`, both overloads | `TrustedImmPtr(arithProfile)` argument when no inline code was generated | the profile's `UCBBinaryArithProfile` or `UCBUnaryArithProfile` | `ImageReference::arithProfile` |
| F3 | `JIT::emitMathICSlow`, both overloads | `TrustedImmPtr(mathIC)` argument | `MathIC` | `ImageReference::mathIC` |
| F4 | same | `TrustedImmPtr(arithProfile)` argument | as F2 | `ImageReference::arithProfile` |
| F5 | `JIT::emit_op_div` | `JITDivGenerator` profile writes; `JITSlowPathCall(slow_path_div)` | A14; A1 | A14; A1 |

The generator's own profile writes inside inline code go through A14.

A MathIC without inline code emits only F2's operation call, and F3 and F4 never run for it (SPEC-image N17). F1 still notes it.

## G. MathIC snippets

A snippet's references are its generator's profile writes (A14) and its branches back to the image, which `JITMathIC::generateOutOfLine` emits through `jumpToImage` and `linkJumpsToImage`; the inline-start rewrite stays native and records nothing itself (SPEC-image section 6.2). A snippet embeds nothing else.

## H. Sites left native

Besides rows C23 to C25 (SPEC-image N29):

- Option-only (rule C3): `AssemblyHelpers::callExceptionFuzz`; the `probeDebug` calls in `JIT::privateCompileMainPass` and `privateCompileSlowCases` (`traceBaselineJITExecution`); the per-bytecode profiler counter `m_compilation` in `JIT::privateCompileMainPass` (`useProfiler`); the early-return counter in `JIT::emit_op_loop_hint` (`returnEarlyFromInfiniteLoopsForFuzzing`).
- Support code (rule C4): every thunk generator, including `JIT::op_enter_handlerGenerator`, `op_check_traps_handlerGenerator`, `op_throw_handlerGenerator`, `valueIsTruthyGenerator`, `valueIsFalseyGenerator`, `consistencyCheckGenerator`, the scope thunk generators of `JITPropertyAccess.cpp` (`generateOpResolveScopeThunk`, `slow_op_resolve_scopeGenerator`, `generateOpGetFromScopeThunk`, `slow_op_get_from_scopeGenerator`, `slow_op_put_to_scopeGenerator`) and `JITSlowPathCall::generateThunk`.
- Literals (rule C5): `JSCell::seenMultipleCalleeObjects()`; encoded `undefined`, `null`, booleans and numbers; `JSValue` tag constants; `TrustedImmPtr(nullptr)`; the `1000` of the `ASSERT_ENABLED` check in `JIT::emitSlow_op_loop_hint`; `abortWithReason` codes; character, int32 and double immediates, among them those of `JIT::emit_compareImpl`; and the constants the arithmetic generators move (`Imm64` doubles, sign masks, `-0` bits).
