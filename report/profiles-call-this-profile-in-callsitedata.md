# profiles.md: the `this` ArrayProfile of three call opcodes moved into the call site's `CallSiteData`

Doc: `skills/reference/knowledge/common/profiles.md`: the Abstract's first paragraph (the counts), item 2, and the `ArrayProfile` row of the table.

## Passages

> `BytecodeList.rb` declares which opcodes carry each profile: 53 opcodes carry at least one, 16 an `ArithProfile`, 28 a `ValueProfile` and 17 an `ArrayProfile`, eight of them both of the last two.

> 2. `ArrayProfile` observes [...] the receiver (`this`) of the calls that carry this profile [...]

Table, `ArrayProfile` row, "where it lives": "opcode metadata".

## What changed

`BytecodeList.rb` no longer gives `call`, `call_ignore_result` and `tail_call` an `arrayProfile` metadata field; their `callLinkInfo` is a `LazyCallLinkInfo`. The receiver's `ArrayProfile` lives in the site's `CallSiteData` beside its `DataOnlyCallLinkInfo` (`CallLinkInfo.h`), so it exists only once the site has its own data (`LazyCallLinkInfo::arrayProfile`, null before; `arrayProfileFor` in `CodeBlock.cpp`). A site without its own data points at a VM-wide shared `CallSiteData`, whose profile both tiers still write and nobody reads (the comment above `class LazyCallLinkInfo`). The writers store through the site's pointer: the LLInt's `arrayProfileForCall` macro (`CallSiteData::m_arrayProfile`) and `JIT::compileSetupFrame` (`LazyCallLinkInfo::offsetOfData`, then `CallSiteData::offsetOfArrayProfile`).

The opcodes stay in `FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE`, and `UnlinkedCodeBlock::allocateSharedProfiles` still counts one UCB array profile per call site, so the positional correspondence holds. `updateAllArrayProfilePredictions` skips a site without its own data but still advances the index.

Recount of `BytecodeList.rb` on the current tree, counting value-profile operands, `ArrayProfile` metadata fields and `profileIndex` operands per opcode, with `op_group`s expanded: 28 opcodes with a value profile (35 slots, unchanged), 14 with an `ArrayProfile` in their metadata, 16 with an arithmetic profile, 7 with both a value and an array profile (`call` was the eighth), 51 with at least one.

## Proposed text

Abstract:

> `BytecodeList.rb` declares which opcodes carry each profile: 51 opcodes carry at least one, 16 an `ArithProfile`, 28 a `ValueProfile` and 14 an `ArrayProfile` in their metadata, seven of them both of the last two; `call`, `call_ignore_result` and `tail_call` keep theirs in the call site's `CallSiteData`.

Item 2, after "the receiver (`this`) of the calls that carry this profile": add "(in the site's `CallSiteData`, which a site gets at its second execution; before that, its writes go to a profile the whole VM shares and nobody reads)".

Table, `ArrayProfile` row, "where it lives": "opcode metadata; for `call`, `call_ignore_result` and `tail_call`, the site's own `CallSiteData`, absent until the site has one".
