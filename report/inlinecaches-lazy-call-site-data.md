# inlinecaches.md: LLInt and baseline call sites get their `CallLinkInfo` at their second execution

Doc: `skills/reference/knowledge/common/inlinecaches.md`: the Abstract, item 2; "Call link info", sections "Fast path" and "Linking"; the `DataOnlyCallLinkInfo` row of the table.

## Passages

> 2. `CallLinkInfo` covers the 14 call opcodes. The LLInt and the baseline share one `DataOnlyCallLinkInfo` per site in the `CodeBlock` metadata. A site links monomorphically on its second slow-path trip [...]

> `DataOnlyCallLinkInfo::initialize` starts the site in `Init`, or in `Virtual` when `useLLIntICs` is off. [...] In `Init`, the first trip sets `seenOnce` and links nothing; the second calls `linkMonomorphicCall` [...]

Table row `DataOnlyCallLinkInfo`, "where it lives": "`CodeBlock` metadata; also inside `InlineCacheHandlerWithJSCall` and Wasm imports"; "who writes it": "`initialize` at link time; [...]".

## What changed

With `useLazyLLIntCallLinkInfos` (default on), ten of the 14 opcodes hold a `LazyCallLinkInfo` in their metadata (`FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO`: `call`, `call_ignore_result`, `tail_call`, `construct`, `super_construct`, `call_direct_eval` and the four iterator opens and nexts). It is a pointer to a `CallSiteData`, which holds a `DataOnlyCallLinkInfo` and, for `call`, `call_ignore_result` and `tail_call`, the `this` `ArrayProfile` (`CallLinkInfo.h`). The four varargs opcodes keep an inline `DataOnlyCallLinkInfo`, initialized at link as before.

- Linking points the site at one of three `CallSiteData`s the VM owns (`VM::neverExecutedCallSiteData`, `executedOnceCallSiteData`, `notExecutedTailCallSiteData`; `LazyCallLinkInfo::setNeverExecuted`, `setTailCallNotExecuted`). They have no owner and never change (`isSharedByUnlinkedCallSites`). The two for ordinary sites store `polymorphicCalleeMask` as the expected callee and `LLInt::unlinkedCall()` as the target (`initializeAsSharedByUnlinkedCallSites`), so the unchanged fast path calls that thunk for any callee. The executed-once one also has `seenOnce` set.
- The thunk enters `LLInt::handleUnlinkedCall`. On the site's first call it moves the site to the executed-once data (`setExecutedOnce`) and calls the callee without linking (`firstCallToJSFunction`, or a virtual call for other callees). On the second it allocates the site's own `CallSiteData` (`LazyCallLinkInfo::ensure`, then `ensureSlow`, which initializes it in `Init` with `seenOnce` already set and publishes it after a `storeStoreFence`), notes `this`'s structure in the new profile, and calls `linkFor`, which links monomorphically. So linking still happens on the second trip.
- Two sites get their own data before their first call. A tail call starts at the tail-call shared data, which looks unlinked (no expected callee); both tiers check the owner field before giving up the frame and call `slow_path_ensure_call_link_info` (LLInt, `prepareCallSiteForTailCall`) or `operationEnsureCallLinkInfoForTailCall` (baseline, `CallLinkInfo::emitFastPathImpl`). In baseline code, a direct eval whose callee is not eval calls `operationEnsureCallLinkInfo` before its virtual call (`JIT::compileCallDirectEvalSlowCase`).
- The baseline call templates load the site's `CallSiteData` pointer from the metadata (`LazyCallLinkInfo::offsetOfData`). The `CallLinkInfo` is at offset 0 of the `CallSiteData`, so that pointer is the IC.
- Readers see only a site's own data: `LazyCallLinkInfo::get` returns null for a shared one, and `forEachLLIntOrBaselineCallLinkInfo` skips the site. GC, `CallLinkStatus` and the status map therefore know nothing of a site that has not run twice. The site keeps its own data for as long as the metadata lives (`~LazyCallLinkInfo` frees it).

## Proposed text

Abstract, item 2:

> 2. `CallLinkInfo` covers the 14 call opcodes. The LLInt and the baseline share one `DataOnlyCallLinkInfo` per site. The four varargs opcodes hold it inline in the `CodeBlock` metadata. The other ten hold a pointer to a VM-wide `CallSiteData` until their second execution, which gives them their own (`LazyCallLinkInfo`) and links them monomorphically; after that a `call` grows a polymorphic stub of up to eight callees, while a `construct` goes to virtual dispatch on its first different callee.

Linking, new first paragraph:

> With `useLazyLLIntCallLinkInfos`, linking points each non-varargs site at a `CallSiteData` the VM shares, whose `CallLinkInfo` has no owner, looks polymorphic and targets `LLInt::unlinkedCall()`. That thunk's handler (`LLInt::handleUnlinkedCall`) moves the site to the VM's executed-once data on its first call, calling the callee unlinked, and on its second gives the site its own `CallSiteData`, in `Init` with `seenOnce` set, and links it through `linkFor`. A tail call, and in baseline code a direct eval of something other than `eval`, get theirs before their first call (`operationEnsureCallLinkInfoForTailCall`, `operationEnsureCallLinkInfo`). Until a site has its own data, nothing reads it: GC and `CallLinkStatus` skip it.

Table row `DataOnlyCallLinkInfo`, "where it lives": "`CodeBlock` metadata for the varargs opcodes, the site's own `CallSiteData` (from its second execution) for the other ten; also inside `InlineCacheHandlerWithJSCall` and Wasm imports". "Who writes it": replace "`initialize` at link time" with "`initialize`, at link time for varargs and in `LazyCallLinkInfo::ensureSlow` for the others".
