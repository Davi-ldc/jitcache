# basics.md: top-level code that ran once releases its CodeBlock

Doc: `skills/reference/basics.md`, section "Lifecycle", subsection "UCB", first paragraph (and the CB subsection, which says a CB dies only when it loses its protection).

## Passage

> `CodeCache` entries can keep the top levels of program, module and indirect eval, and the UFE, not the UCB, of a `new Function`.

## What changed

With `useRunOnceCodeRelease` (default on; a fork option declared in the `USE(BUN_JSC_ADDITIONS)` option list), top-level code lets go of its code right after it runs, if the CB never left the LLInt and no baseline compilation of its UCB is queued (`GlobalExecutable::canReleaseLinkedCodeNow`).

- A program: `Interpreter::executeProgram` calls `clearCode` with `ClearCode::All` after `vmEntryToJavaScript` returns. The executable drops its CB and its UCB reference; the UCB stays alive through its `CodeCache` entry, if it has one.
- A module: `ModuleProgramExecutable::didFinishEvaluation` waits until every record that runs the code has finished (`m_recordsYetToFinishEvaluation`) and skips code that several loaders' records share (`m_isShared`). It then calls `clearCode` with `ClearCode::KeepWhatNeedsParsing`, which drops the CB and calls `releaseUnlinkedCodeIfRecoverable`: a UCB decoded from a persistent payload (`cachedPayloadIndex()` non-zero) leaves the executable and its `CodeCache` entry (`CodeCache::forgetUnlinkedModuleProgramCodeBlock`) and is decoded again if needed (`ModuleProgramExecutable::getUnlinkedCodeBlock`). A generated module UCB stays.
- Functions the top-level code created keep their own executables; only the top-level CB and UCB go.

## Proposed text

Add after the quoted sentence:

> With `useRunOnceCodeRelease`, top-level code whose CB never left the LLInt drops that CB once it has run (`canReleaseLinkedCodeNow`): a program right after its run, together with its UCB reference, which a `CodeCache` entry may still hold; a module once every record running it has finished, and its UCB too when that came from a persistent payload, leaving the `CodeCache` as well (`releaseUnlinkedCodeIfRecoverable`).

Net size change is about +390 bytes; the doc is at 39,973, so condense elsewhere first.
