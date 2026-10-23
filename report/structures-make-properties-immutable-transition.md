# structures.md: the `MakePropertiesImmutable` transition and the `hasImmutableProperties` bit

Doc: `skills/reference/knowledge/common/structures.md`: the `[^operation]` footnote and the `m_bitField` row of the Layout table.

## Passages

> There are eighteen `TransitionKind` values, and four use a name or symbol as the key [...] Another twelve use a null key: [...] plus `PreventExtensions`, `Seal`, `Freeze` and `BecomePrototype`. [...] `PreventExtensions`, `Seal` and `Freeze` set `didPreventExtensions`; [...]

> `m_bitField` | 22 fields, three of them wider than one bit (`dictionaryKind`, `transitionKind`, `definitelyNonThenableState`) [...]

## What changed

- `TransitionKind` (`StructureTransitionTable.h`) has a nineteenth value, `MakePropertiesImmutable` (18), with a null key: the object becomes non-extensible and its own properties and prototype can no longer change, while attributes stay as they are. `Structure::makePropertiesImmutableTransition` creates it through `nonPropertyTransition`. `preventsExtensions` is true for it, so it also sets `didPreventExtensions`. For a `JSArray` with `Int32`, `Double` or `Contiguous` elements it adds `CopyOnWrite` to the indexing type (`newIndexingType`).
- `Structure::m_bitField` gains `hasImmutableProperties` (`DEFINE_BITFIELD` list in `Structure.h`, 23 entries now). The transition sets it and every later transition inherits it. The transition also clears `transitionWatchpointIsLikelyToBeFired`, since the object will not change shape again, and keeps the parent's `didTransition` (`Structure::nonPropertyTransitionSlow`).
- The entry point is `JSObject::makePropertiesImmutable` (exported; no caller inside the engine besides its own `JSGlobalProxy` forwarding). Before transitioning it moves indexed storage where no compiled code writes in place: copy-on-write storage for a hole-free `JSArray` (`useCopyOnWriteArraysForImmutableProperties`, default on), dictionary indexing mode otherwise. For a global object it makes the symbol table's entries read-only and fires `varReadOnlyWatchpointSet`; for a `RegExpObject` it makes `lastIndex` non-writable and fires the realm's `regExpLastIndexWritableWatchpointSet`.

## Proposed text

Footnote: replace "There are eighteen `TransitionKind` values" with "There are nineteen", "Another twelve use a null key" with "Another thirteen use a null key", and "plus `PreventExtensions`, `Seal`, `Freeze` and `BecomePrototype`" with "plus `PreventExtensions`, `Seal`, `Freeze`, `BecomePrototype` and `MakePropertiesImmutable`, which fixes the object's own properties and prototype without changing attributes (`JSObject::makePropertiesImmutable`)". Replace "`PreventExtensions`, `Seal` and `Freeze` set `didPreventExtensions`" with "`PreventExtensions`, `Seal`, `Freeze` and `MakePropertiesImmutable` set `didPreventExtensions`; the last also sets `hasImmutableProperties`, which every later transition inherits".

Layout, `m_bitField` row: "23 fields" instead of "22 fields", and add `hasImmutableProperties` to the listed bools.
