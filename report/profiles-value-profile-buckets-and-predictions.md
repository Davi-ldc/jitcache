# profiles.md: metadata value profiles are 8-byte buckets with a lazily allocated predictions array

Doc: `skills/reference/knowledge/common/profiles.md`: the Abstract, item 1; section "Drain", first paragraph; the `ValueProfile` row of the table.

## Passages

> 1. `ValueProfile` stores the `JSValue` produced by the instruction. A drain classifies it, accumulates its type in `m_prediction` and clears the slot (`m_buckets[0]`) in the same step.

> For the value profile, `ValueProfileBase::computeUpdatedPrediction` classifies the sample, merges the type into `m_prediction` and clears the slot.

Table, `ValueProfile` row, "where it lives": `CodeBlock` metadata; "what it observes": "the raw `JSValue` produced, plus the prediction accumulated by the drain".

## What changed

A metadata value profile is no longer a `ValueProfile` struct with its own `m_prediction`.

- `MetadataTable`'s layout is `[ValueProfile buckets][LinkingData][offsets][content]` (comment above `class MetadataTable`): one 8-byte `EncodedJSValue` per value profile, in front of the table, addressed backwards from `valueProfileBucketsEnd()` by profile offset.
- The predictions are a separate `SpeculatedType` array, one per profile, held by `LinkingData::valueProfilePredictions`. With `useLazyValueProfilePredictions` (default on) it is null until `MetadataTable::ensureValueProfilePredictions` allocates it, the first time a merge has a type other than `SpecNone` (`ValueProfileRef::mergePrediction`). The allocation is zeroed and published by compare-and-swap, because the mutator and compiler threads can race to it. With the option off, the `MetadataTable` constructor allocates it.
- Readers and drains go through `ValueProfileRef` (`MetadataTable.h`), which pairs the bucket with the table: `prediction()` returns `SpecNone` while the array is null, and `computeUpdatedPrediction` classifies the bucket, merges, and clears it. `MetadataTable::forEachValueProfile` reads the array pointer once per walk.
- Argument profiles are unchanged: `ArgumentValueProfile` (a `ValueProfileBase` with a speculation-failure bucket) keeps its own `m_prediction` in `CodeBlock::m_argumentValueProfiles`.

## Proposed text

Abstract, item 1:

> 1. `ValueProfile` stores the `JSValue` produced by the instruction in an 8-byte bucket in front of the metadata table. A drain classifies it, merges its type into the table's predictions array and clears the bucket in the same step (`ValueProfileRef::computeUpdatedPrediction`). That array, one `SpeculatedType` per profile, is allocated only when the first merge has a type to record (`MetadataTable::ensureValueProfilePredictions`, `useLazyValueProfilePredictions`); until then every prediction reads `SpecNone`.

Drain, first paragraph: replace "`ValueProfileBase::computeUpdatedPrediction` classifies the sample, merges the type into `m_prediction` and clears the slot" with "`ValueProfileRef::computeUpdatedPrediction` classifies the sample, merges the type into the table's predictions array and clears the bucket (argument profiles keep their own `m_prediction`)".

Table, `ValueProfile` row, "where it lives": "`CodeBlock` metadata: a bucket in front of the table and a slot in its predictions array, allocated on the first merge".
