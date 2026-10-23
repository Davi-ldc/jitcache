# baseline/compiler.md: call templates reach the `CallLinkInfo` through the site's `CallSiteData` pointer

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "Templates that leave records", paragraph "Calls", and the per-`CodeBlock` state table under "Reaching per-CodeBlock state".

## Passage

> Calls. Each call opcode keeps its `CallLinkInfo` in the opcode's metadata, shared with the LLInt. Except in `call_direct_eval`, which first calls an operation and only falls back to a virtual call, the fast path loads the expected callee, the target and the callee's `CodeBlock` from it.

Table row: "an opcode's metadata: LLInt caches, array and allocation profiles, `CallLinkInfo`, scope metadata | `metadataTableRegister` plus that opcode's metadata offset [...]".

## What changed

`JIT::compileOpCall`: for the four varargs opcodes, whose metadata holds a `DataOnlyCallLinkInfo` inline, the template still materializes its address (`materializePointerIntoMetadata`). For the other ten (`LazyCallLinkInfo`), it loads the site's `CallSiteData` pointer from the metadata (`LazyCallLinkInfo::offsetOfData`) into `callLinkInfoGPR`; the `CallLinkInfo` is at offset 0 of the `CallSiteData`. Until the site's second execution that pointer is a VM-wide shared `CallSiteData` whose target is the unlinked-call thunk (inlinecaches.md has the report). Three more changes:

- The `this` profile write of `call`, `call_ignore_result` and `tail_call` (`JIT::compileSetupFrame`, under `shouldEmitProfiling`) loads the same pointer and stores the structure ID at `CallSiteData::offsetOfArrayProfile`. `iterator_open` keeps its profile in its metadata.
- `JIT::compileCallDirectEvalSlowCase`: when the callee is not eval, the slow case tests the owner of the site's `CallLinkInfo` and, if it has none, calls `operationEnsureCallLinkInfo` (with the frame's `CodeBlock` and the bytecode index) before the virtual call, which needs a call type.
- The tail-call path of `CallLinkInfo::emitFastPathImpl` tests the `CallLinkInfo`'s owner and, for a site still on the VM's tail-call data, moves `operationEnsureCallLinkInfoForTailCall` into `nonArgGPR0`, calls it and continues with the returned `CallLinkInfo`, all before giving up the frame.

## Proposed text

Calls:

> Calls. Each call opcode's `CallLinkInfo` is shared with the LLInt: inline in the metadata for the four varargs opcodes, and for the others in a `CallSiteData` the metadata points to, a VM-wide one until the site's second execution. The template loads that pointer, which is also the `CallLinkInfo`'s address, and writes the `this` profile of `call`, `call_ignore_result` and `tail_call` through it. Except in `call_direct_eval`, which first calls an operation and only falls back to a virtual call, after giving the site its own `CallSiteData` (`operationEnsureCallLinkInfo`), the fast path loads the expected callee, the target and the callee's `CodeBlock` from it. A tail call whose site has no data of its own yet calls `operationEnsureCallLinkInfoForTailCall` before giving up its frame.

Table row: "an opcode's metadata: LLInt caches, array and allocation profiles, `CallLinkInfo` or the pointer to the site's `CallSiteData`, scope metadata".

Net size change is about +420 bytes; the doc is at 39,109 bytes, so condense elsewhere first.
