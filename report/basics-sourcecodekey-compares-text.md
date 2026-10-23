# basics.md: SourceCodeKey equality compares the source text again

Doc: `skills/reference/basics.md`, section "How UCBs and CBs relate", second paragraph, last sentence.

## Passage

> Under `USE(BUN_JSC_ADDITIONS)`, `SourceCodeKey::operator==` stops at these tests, without comparing the source text.

## What changed

`SourceCodeKey::operator==` (`parser/SourceCodeKey.h`) has no `USE(BUN_JSC_ADDITIONS)` branch. After the hash, length, flags, `m_functionConstructorParametersEndPosition`, `m_name` and `host()` tests, it requires `m_sourceCode == other.m_sourceCode || string() == other.string()`: the same provider and range (`UnlinkedSourceCode::operator==` is defaulted over `m_provider`, `m_startOffset`, `m_endOffset`), or else equal text.

## Proposed text

> Two keys with the same provider and range are equal without reading the text; otherwise `SourceCodeKey::operator==` compares the text too.

Net size change is about +10 bytes.
