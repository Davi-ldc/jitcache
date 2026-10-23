# baseline/compiler.md: value-profile sites store into 8-byte buckets before the metadata table

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "Reaching per-CodeBlock state", the table row for value profiles.

## Passage

> | value profiles | `metadataTableRegister` minus an offset: they sit before the table in the same allocation |

## What changed

The row is still right about the addressing, but what sits there changed: each metadata value profile is now a single 8-byte `EncodedJSValue` bucket, and its prediction lives in a separate array the `MetadataTable` allocates lazily (`MetadataTable::ensureValueProfilePredictions`). `JIT::emitValueProfilingSite` stores the value at `-(valueProfileOffsetFor(...) × sizeof(EncodedJSValue)) - sizeof(UnlinkedMetadataTable::LinkingData)` from `metadataTableRegister`, so the stride baked into the code is 8 bytes and the predictions never appear in it.

## Proposed text

> | value profiles | `metadataTableRegister` minus an offset: one 8-byte bucket each, before the table's linking data in the same allocation; their predictions live in a separate array the code never touches |

Net size change is about +100 bytes; the doc is at 39,109 bytes.
