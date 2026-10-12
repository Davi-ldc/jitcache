spec-conflict

# The pin comparison's identity rule leaves addresses in some names

## Requirement

`docs/JitCache/specs/SPEC-integrator.harness.md`, section 11.3 (Canonical code), the Allocations paragraph. The paragraph has no rule ID:

> Each header gives an allocation's range and name. The name with every hexadecimal number replaced by `0x?`, together with its ordinal among the run's allocations of that name, is the allocation's identity across runs.

Section 11.2 (Runs) describes the header as the one line `Generated JIT code for <name>: [<start>, <end>) <size> bytes`.

## Why the code cannot meet it as written

The rule replaces hexadecimal numbers so that an identity holds no address, but some names print addresses in two other forms.

- A DFG or FTL OSR exit's name dumps its operands through `ignoringContext<DumpContext>`, which builds a fresh `DumpContext` for each name. That context names each Structure `%<id>:`, and `StringHashDumpContext::getID` derives the id from the hash of `Structure::dump`, which prints the Structure's address and its prototype's. The id therefore differs between any two processes. In the kept `global-var-order.js` pair, exit #20 shows `%AR:global` in this build and `%C5:global` in the pin.
- `SlowPathCallKey::dump` prints `m_indirectOffset`, which shares a union with `m_callTarget`. For a direct call it is the low 32 bits of the operation's address, printed in decimal. It stays the same across runs of one build and differs between this build and the pin: `tdz-scale.mjs` prints 68340976 in one and 66387312 in the other.

With only hexadecimal numbers replaced, the comparison reports `pin-allocations` at the first such allocation even though both builds finalized the same one.

The header form itself does not conflict, but section 11.2 does not say that a name can span lines. `AccessCase::dump` starts each of an access stub's cases on a new line, and `InlineCacheCompiler` ends a stub's name with `listDump(keys)`. A CodeBlock's name starts with its function's `ecmaName`, which can hold a newline, and both `JITDisassembler::dumpHeader` and `JIT::link`'s `FINALIZE_CODE` name print it. `YarrDisassembler::dumpHeader` runs from a link task. It prints its own `Generated JIT code for <variant> /<pattern>/<flags>:` line with the pattern unescaped, then `    Code at [s, e):` and the listing, and only after that comes the allocation's own header, which ends in `.`.

## What the code does instead

`Tools/Scripts/jitcache-pin-compare.ts`:

- `identityName` replaces Structure ids (`%[A-Za-z0-9]{2,5}:` becomes `%?:`), then the decimal after `indirectOffset = `, then hexadecimal numbers. Ids go first because one can hold a hexadecimal number: `integerToSixCharacterHashString` starts a 32-bit hash's id with a letter from A to E, so `%B0xa:` can occur. The decimal StructureID that `Structure::dump` prints stays in the identity, since it is an offset into the structure heap and repeats whenever allocation order repeats.
- `parseAllocations` ends an allocation header at the first line that ends in its range and matches the joined lines. It ends a baseline dump header at the first line that ends in `, instructions size = N`, before the `   Source:` line. The search stops at the next header, so a header that never reaches its end is still output the tool cannot read.
- A `Generated JIT code for` line that ends in `:` and is followed by a four-space `Code at` line before any range is Yarr's own header. The parser skips it, and the tool does not read the listing that follows. Yarr's allocation is still compared by identity through the header after it.
- The `pin-allocations` message JSON-quotes both identities, since they can now hold newlines.

This asks the following of other tasks:

- Whoever maintains the integrator SPEC should widen section 11.3's rule to "the name without every address it prints: hexadecimal numbers, `StringHashDumpContext` ids and `SlowPathCallKey`'s `indirectOffset`". Section 11.2 should say that a name can span lines, where each header ends, and that Yarr prints a header of its own before the allocation's. H7's list of crafted pairs in section 13 can name the seven new cases.
- 38 corpus comparisons still fail for reasons outside this item. In 32 of them (`integrator/exec-alloc-faults.js` and `exec-alloc-ic-marker.js`), an access stub reads at `offset = 206` against the pin's `195`, because this build's shell global object holds 11 `jitcache*` functions the pin's lacks. Five (`cb/body-kinds.js`, `cb/realm-step.js`, `integrator/llint-off.js`, `ucb/feedback-roundtrip.js`, `ucb/root-holder.js`) finalize thunks or a MathIC snippet in a different order or number. `cb/families.js` differs in code at body 32, block `main:679` (`resolve_scope`). Each of these scripts fits the cause that SPEC-integrator-history's "The pin comparison runs without the shell functions" names. Section 11.2's remedy, `--jitcache-test-no-shell-functions`, is not in `jsc.cpp` or `Tools/Scripts/run-jitcache-tests` yet.

## Evidence

- Engine: `LinkBuffer::finalizeCodeWithDisassemblyImpl`, `InlineCacheCompiler` (both access-stub names), `AccessCase::dump`, `YarrDisassembler::dumpHeader`, `JITDisassembler::dumpHeader`, `CodeBlock::inferredName`, `ignoringContext<DumpContext>`, `StringHashDumpContext::getID` (`wtf/StringHashDumpContext.h`), `integerToSixCharacterHashString`, `Structure::dump`, `Structure::dumpBrief`, `SlowPathCallKey::dump` (`ftl/FTLSlowPathCallKey.h`).
- Tool: `identityName`, `headerEnd`, `parseAllocations` and `compareRuns` in `Tools/Scripts/jitcache-pin-compare.ts`.
- Crafted cases in `JSTests/jitcache/integrator/runner/pin-compare-tests.ts`: a name that spans lines, Yarr's own header, Structure ids with the decimal offset, a name that holds another offset, a Structure of another class, a body whose name spans lines, and a header that never reaches its range. All 50 pairs pass. The original tool fails five of the seven new cases, every one except the two it already reported correctly. A tool that replaces hexadecimal numbers before ids, or never replaces ids, fails the Structure id case. A tool that recognizes only single-line dump headers compares a body whose name spans lines by identity alone, so the body case fails.
- Kept output: all 338 stderr files of the 169 corpus pairs that V4's runner found different, and the 126 files of H7's self-comparisons, parse without an unreadable header. The proposer's probe (`v4-probe/self-results.tsv` under the c1-r1 ladder's run directory) shows all 126 H7 self-comparisons the same.
