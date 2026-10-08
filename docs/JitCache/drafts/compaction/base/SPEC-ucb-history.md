# SPEC-ucb history

Rationale and review records for [SPEC-ucb.md](SPEC-ucb.md) and [SPEC-ucb.codec.md](SPEC-ucb.codec.md). Nothing here binds an implementer.

## Design record, first draft (2026-10-05)

Written from the current THREAD with nothing at the SPEC's paths.

### The bytecode cache's codec, in a JITCache mode, rather than a new one

A UCB core is the whole output of generation, and the bytecode cache already encodes every part of it, including cells that are hard to transport (symbol tables, template descriptors, immutable butterflies, big integers) and the child UFEs with their rare data and TDZ links. Bun maintains that codec, and its rewrite made the format deterministic. A JITCache codec of its own would duplicate several thousand lines and drift from the engine. The cost is surgery at five points, each closing a gap the earlier work found: child bodies (F7), external strings (off), borrowed streams (F7's assertion), checksums that a persistent or option-disabled decoder skips (F8, covered by the container), and UFEs that decode as cache-born (F9).

### The core digest instead of a separate index-space walk

THREAD asks strict to check a reused UCB "against the digest of the captured core". The first idea was a canonical walk over the index spaces other lanes use. Once the codec is deterministic, its encoding is that canonical form: it covers every index space and everything else observable in the core, so `coreDigest` is the SHA-256 of the section and both strict checks (round trip and reuse) are "encode and hash". The determinism argument is in the codec file, section 6, and twins check it on every import.

### Global var order and `InlineMap` layouts

`CachedInlineMap::decode` reserves capacity and re-adds entries, which reorders a hashed map. `ProgramExecutable::initializeGlobalProperties` creates global vars in that order, so a program with more than nine vars imported through the native codec would enumerate `globalThis` differently from a JITCache-off run. This is a native bytecode-cache property, but JITCache would introduce it into runs that generate natively, so the JITCache mode transports the exact bucket layout. Reproducing a layout by choosing an insertion order is not possible in general (growth rehashes, tombstones), so `InlineMap` gains a checked layout restore. The record grows for the native cache too, which keeps one record shape and costs a few bytes per hashed map.

### Import before the provider's cached bytecode (superseded by revision 1)

THREAD has the import replace generation. At the CodeCache it also replaces Bun's provider decode, because nothing observes that decode and the import carries feedback and an image the decode lacks. Where a decode cannot be replaced, the lane treats the decoded UCB as live: Bun's `vm.compileFunction` decode, which THREAD keeps for `cachedDataRejected`, and the lazily decoded slots of a UFE, whose call slot shares storage with the decoder (F1).

### Meeting a live UCB (the miss flag superseded by revision 1)

The lane reads "an import that meets a live UCB for its body ... reuses it" as an import at a request point whose holder already holds a UCB. The other reading, a global lookup of live UCBs by key, would hand one UCB to two holders, which native code never does and which THREAD Identity rules out ("Native UCB and CB ownership and sharing stay as they are"). Attaching reuses the image path only; seeding a published UCB is forbidden. The attach is skipped when the sharing slot holds code (that code wins), when a pending import exists, and after a miss for that UCB, so steady state costs one registry lookup.

### Contexts hold every generation input outside the key (the child case refined by revision 2)

THREAD's list of context parts is given as examples. A generation input that is in neither key nor context would let two different UCBs share a body, so the context also takes the script mode, derived and eval context types, the arrow-context bit and a direct eval's flags. Every one of them is constant for most requests, so the extra misses are rare. A child's context needs only its offset, because its parent's key and context pin everything else.

### Children's parse fields and learned bits

Native generation creates child UFEs with zero features and no captured variables; they change when the child's body is parsed. A producer's parent, encoded after some children ran, would carry parsed values, so the JITCache mode writes them unparsed and each child's own body restores its fields at its own request (F3 makes that restore load-bearing). The singleton-invalidated bit is the opposite case: learned at run time, held by the UFE, and read when the parent links, so it travels with the parent.

### The LLInt counter (the formula superseded by revision 2)

"Travels as native finalization leaves it" and "arms it ... without a slice" are read together: the capture records T and P as they are, which right after finalization is `jitSoon`'s state, and the import re-arms with no CodeBlock, as `UnlinkedCodeBlock`'s constructor does. The formula reproduces `ExecutionCounter::setThreshold` without a CodeBlock from public fields, so no native function changes and the CB lane's own counter work in `ExecutionCounter` stays independent.

### Lazy child identities

A child's identity digest is computed at its first request, from the parent key stored with the UFE. Most nested functions of a large module never run, and eager digests would cost the native generation path one SHA-256 per child.

### A lock on a VM-thread registry

Sweeping happens on the thread that holds heap access (F12), so the registry is touched by one thread at a time. The lock is still needed: a sweep can run a destructor in the middle of a JITCache step on that same thread, and the lock's rules (a leaf, no cell allocation or GC-deferral change inside) make that reentrancy safe and explicit.

### SHA-256

The engine has none (F13). THREAD mandates SHA-256 for source digests, so the lane owns an implementation with SHA-NI and ARMv8 paths, because hashing every root source is on the request path even though THREAD measures it separately.

### String constants and atom-ness

The Image lane's index-space requirement asks for string constants atom-backed exactly where the producer's were. Generation always makes them atoms; the native decoder deliberately does not for long strings, and the baseline's strict-equality templates emit their constant fast path only for atoms. An image compiled against atoms and installed on a UCB with non-atom constants would compare pointers that can never match. The codec therefore writes atom-ness explicitly (E10), the identity section carries an atom map so a live UCB can be compared without decoding, and a live UCB that disagrees is a miss rather than invalid material, because a natively decoded UCB disagrees legitimately.

### Smaller points

- A failed symbol lookup during decode becomes a decoder failure instead of a `RELEASE_ASSERT`, because THREAD requires everything received to be verified and a body naming a private name this VM lacks must be invalid material, not a crash.
- The import reports the extra memory generation reports, so GC pacing does not depend on whether a body was imported.
- THREAD names "the UFE slot or the CodeCache" as holders; a direct eval's holder is its `DirectEvalExecutable`, which publishes the same way.
- Bun's internal modules can be created (`createBuiltinExecutable`) or decoded (`decodeBuiltinFunction`); both hash the provider's whole source, so their identities agree as long as Bun's source spans the provider, which `makeInternalModuleSource` does today. A mismatch would only cost hits.

### THREAD gaps raised

1. Parse-time stack overflow (SPEC section 6.3.5): an import cannot reproduce a `RangeError` the parser would raise at a given stack depth. Provisional: import regardless, as a native CodeCache hit does.
2. Owner of UFE-held learned state (section 8.1): THREAD's lanes do not name it. Provisional: this lane carries `m_singletonHasBeenInvalidated`.
3. `vm.compileFunction` with `cachedData` (section 6.2.6): the user function's UCB usually arrives in a lazily decoded slot, so its "import" is an attach without UCB seeds. Provisional: attach only. (Settled by the human after the run: the wrapper gets its key and the user's function imports at its first call; revision 1 implements it.)

## Design record, revision 1 (2026-10-05)

Written after review round 1. Every blocker and major finding of that round held up against the code; the record of each is below. The decisions that changed the design:

### Decode points keep the native decode and seed its result

The first draft served a lazily decoded UFE slot as a live UCB, attached without seeds, on the premise that `decodeCachedCodeBlocks` publishes both slots before an import could run. The code says otherwise: the decoded UCBs sit in the slots unpublished until `m_isCached` clears, since the marker skips the slots while it is set and nothing else reads them (SPEC F1). Two fixes were possible. Replacing the decode of the requested slot with an import follows THREAD Restoration's wording, but the imported UCB's children come from the JITCache codec, which writes no child bodies, so every descendant the producer never captured would be generated from source where the JITCache-off run decodes it cheaply; for Bun's `--compile --bytecode` programs that is a startup regression the lane would introduce. Keeping the decode and seeding the decoded UCB before `m_isCached` clears gives the holder exactly the native UCB, keeps the descendants lazily decodable, and costs what the import would. The same reasoning moved the CodeCache's provider decode ahead of the import: the provider's block decodes natively and is seeded before `addCache`, and the import only replaces generation. Because THREAD Restoration says the request point reads the core from the body file, the choice is marked Provisional (SPEC section 6.3.3).

### Provenance is part of the context (superseded by revision 2)

A natively decoded UCB differs from a generated one: hashed declaration maps re-added in encoded order can iterate differently (F10), and its string constants are atoms only where its decoder had already atomized the string. Importing a body captured from a decoded UCB where the native path generates would therefore change `Object.keys(globalThis)` order against the JITCache-off run, and matching a generated body against a decoded UCB would fail strict's digest and turn the cache off for a legitimate deployment difference. THREAD Identity already says each body records the context it was generated under and a differing context misses; the lane counts how the UCB was produced as part of that context, so both cases become ordinary misses.

### Atom-ness of natively decoded constants depends on decode history (superseded by revision 2)

Verifying the provenance argument turned up a further fact: `CachedUniquedStringImplBase::decodePlainString` returns an atom when the decoder already atomized the same string, and an embedder's shared string table can later swap a constant to the atom (`DecoderStringTable::atomFor`). Two runs of one bytecode-cached program that first call their functions in different orders thus decode some constants with different atom-ness. Check C8 stays a miss for decoded and live UCBs, and the codec now writes atom-ness only for constant registers, the only place an image reads it; elsewhere it would make two native decodes of one payload encode differently and fail strict spuriously (SPEC-ucb.codec.md, E10 and section 6).

### Misses expire with the body version

The first draft's `attachMissed` never expired, which contradicted its own `live-attach.js` and kept a ConsumerProducer VM from reusing a UCB whose body it committed itself. A miss now stamps the body version it saw (the commit identifier THREAD Maintenance defines, read from the integrator's index), and a later request reads the artifact again only when that version changed. Absent bodies are retried once committed; a mismatching body is not revalidated until replaced.

### Scope singletons and root singleton bits

`SymbolTable::notifyCreation` propagates a clone's singleton invalidation into the UCB's own table, and later clones start invalidated; the bytecode cache writes none of it. The feedback section gained one bit per constant. A Function-constructor or builtin root's own `m_singletonHasBeenInvalidated` is read by `link` right after the root UFE is created, before any body request, so the lane reads it from the root's call (or construct) body at creation; bodies of every captured root have the empty code-generation mode, so the key is known there. Both extend the existing Provisional on UFE-held state.

### Interfaces frozen by a header task

The first draft froze interfaces in prose but left the engine entry points, the section functions, the request state, statistics and the twin helpers undeclared, and its task order had a cycle (the codec's builtin hook needed a header the call-site task wrote after the engine task). The revision declares every header and adds task 0, which writes them all before any implementation task starts. The lane's engine makes its own twin calls; the integrator only provides the sink.

### Direct eval site is optional

`DebuggerCallFrame::evaluateWithScopeExtension` also calls `DirectEvalExecutable::create`. The site became a defaulted pointer, null meaning no key, so that file keeps its call and no task edits it; a debugger attach turns cache activity off anyway.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged from the first draft.
2. Owner of UFE-held learned state (section 8.1): extended to a root's own bit, read at the root's creation.
3. Decode points (section 6.3.3): the native decode stays and its unpublished result is seeded, where THREAD Restoration's wording reads the core from the body file.

## Design record, revision 2 (2026-10-05)

Written after review round 2, against the THREAD that settles the first run's gaps. Every blocker and major finding of that round held up against the code; three of them reported one defect. The decisions that changed the design:

### Request features are snapshotted

Keys read the executable's `lexicallyScopedFeatures()` lazily, but the request's own native steps overwrite that field with the parse's: generation through `generateUnlinkedCodeBlockImpl`, a CodeCache hit through `recordParseFromUnlinkedCodeBlock` (SPEC F18). A Producer computed its first key after generation and recorded a `"use strict"` program under strict 1, while a Consumer computed it before generation under strict 0, so the body never imported; a hit replaced the request's with-scope bit with the cached UCB's. The request objects now snapshot the features in their constructors, which run before any native step and see the value the native `SourceCodeKey` is built from, and keys read only the snapshot. The twin generator passes the snapshot too, since an import has already overwritten the executable when the twin runs.

### Settlement covers the requested slot only

Recording the other decoded slot settled the request, so `new F()` on a function cached for call left the generated construct UCB unrecorded, its children without identity. `recordDecodedSlot` now neither reads nor sets `settled`; I19 says a request settles on the UCB it asked for.

### Embedder payloads are matched by content

Two facts broke the premise that equal key and context make a natively decoded UCB the body's. Bun's `vm.compileFunction` generates `cachedData` tainted by the global scope extension it sets and never clears, while its run path generates the wrapper untainted (F11), and the decode's `SourceCodeKey` check ignores the with-scope bit (F4). Nothing records the options a payload's generator ran with either. So a decoded UCB now matches only by its core digest, compared whether or not strict is on, and a difference is a miss; S2 stays the strict check for generated and imported UCBs, whose content does follow from their inputs. A decoded root is recorded under the key of the request its payload was generated for, with the payload's with-scope bit, so its children's keys pin their UFEs; a request whose bit differs misses, as THREAD says of a CodeCache hit. That reading is marked Provisional.

### Provenance leaves the context; atom-ness leaves the core bytes

The first revision put provenance in every context and wrote each constant register's atom-ness into the core, so a decoded function never matched a generated body, and two decodes of one payload rarely matched each other: lazy decoding makes most long constants plain strings, and property-key uses atomize them later in place (F19). Now the codec decodes every string constant register as an atom, as generation makes them, and writes no atom-ness; the identity section's atom map carries the producer's atom-ness instead. Seeding atomizes the marked constants of the unpublished decoded UCB with `JSString::swapToAtomString`, which allocates no cell and which JSC's own decoder string table already uses, so the image's constant-string fast paths find atoms. Provenance became a field of the identity section, checked only for programs and modules, whose hashed declaration maps a decoder lays out differently from generation. The guarantee to the Image lane is now one-directional: every constant that was an atom at capture is an atom at install. No UCB can promise "exactly", so that difference from SPEC-image's R-UCB-1 is marked Provisional.

### The holder UFE pins a function body's generation inputs

With decoded parents keyed by their payload's bits, a child's key still pins its UFE only if the payload's generator ran with the fixed option values. Check C11 closes that: wherever a match relies on generation inputs (an import, an attach to a generated or imported UCB), the requesting UFE's descriptor digest must equal the one the producer's body was generated from, else a miss. The descriptor is the codec's encoding of the UFE alone, which E1 and E2 make independent of its slots and its own parse; it also replaces the hand-listed creation digest of root contexts.

### Attaches cost nothing until an attach can happen

The attach computed the full request key, a whole-source SHA-256, at every CodeCache hit on a recorded UCB without parked code, and re-encoded the UCB for S2 at every re-attach. The record now keeps its key's root bits and the body version it was matched at. The attach checks the sharing slot, the index version and the stamp first, compares the bits, computes the source digest only when an attach would follow, skips the key for UFE bodies, whose record is their identity by construction, and re-attaches at a matched version without re-checking. A pending import the `shouldJIT` gate drops stamps its version as missed, since the gate would drop it again.

### The LLInt counter keeps its progress exactly

The arming copied `setThreshold`, which truncates the slice into `m_counter` but adds the untruncated slice to `m_totalCount`, so `count()` gained up to one point; `jitSoon` arms against a fractional memory-pressure multiplier, so the progress after finalization is fractional and T4 would fail on most bodies. The arming now truncates before adding, which keeps `count()` at P and crosses after the same points as native.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): unchanged.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): a decoded root takes the key of the request its payload was generated for.
5. `vm.compileFunction` (section 6.2.6): THREAD's "the user's function imports at its first call" holds only when the producer's wrapper had the consumer's key, which Bun's default parsing context prevents for a producer that ran without `cachedData`.
6. Atom-ness guarantee (section 10.1): every constant that was an atom at capture is one at install, rather than "exactly where the producer's were".

## Design record, revision 3 (2026-10-05)

Written after review round 3. All four blocker and major findings held up against the code; one part of a suggested fix did not. The decisions that changed the design:

### Contexts record the source's first line

The lexer numbers lines from `SourceCode::firstLine()`, and most of a UCB stores lines relative to its source, which is why the native CodeCache can serve one UCB to sources at different lines (`SourceCodeKey` holds no position). Three fields of the child descriptors are absolute: a class constructor's class source, the positions of class elements, which a field initializer's parse restores into the lexer, and the initializer's first-line offset, which comes from a default token location and so equals minus the parent's first line (SPEC F21). Two `vm.Script`s with one text and different `lineOffset` values therefore produced different UCBs under one key and context. An import then reported the producer's lines from field initializers and static blocks, or a line near 2^31 when the consumer's start line was lower, and strict turned the cache off at an attach. Every context now records the first line of the source its body was produced from. A child records its own, since its key carries the parent's key but not the parent's context. A field initializer's linked source starts on line 1 under any first line, because its offset cancels its parent's line and the `SourceCode` constructor clamps the wrapped sum, so its context cannot tell two first lines apart; its holder UFE's element positions differ instead, and C11 misses.

The finding also suggested the start column. It shapes no UCB: the lexer measures columns from the line start, which on the first line is the source start; `Parser::parse` passes the source's column only to the root node, whose start column `generateUnlinkedCodeBlockImpl` uses only for the executable's end column; `recordParseFromUnlinkedCodeBlock` rebases the global end column on the request's source; and `UnlinkedFunctionExecutable::linkedStartColumn` adds a parent's column at link. Recording it would add misses and fix nothing, so the SPEC's Notes refute that part.

Rebasing the absolute lines at decode was the alternative. It would keep hits across start lines, but the decoder would need the request's `SourceCode`, the encoder would need to rewrite three fields relative to a base that the initializer's clamped source hides, and a decoded UCB would no longer encode to its payload's bytes. A start line that differs between producer and consumer for one text is rare outside `vm.Script` with `lineOffset`, so a miss costs little.

### Imports rebuild the butterflies generation builds from atom strings

`ArrayNode::emitBytecode` gives an all-atom-string literal the structure `cellButterflyOnlyAtomStringsStructure` and makes each element the VM's canonical `JSString` for its atom; `Array.prototype.indexOf` and the DFG and FTL lowerings of `indexOf` and `includes` take a pointer-comparison fast path on it. The codec's decode gives every butterfly the plain structure and plain strings, so an import differed from its native twin. Writing the form into the core bytes would have made a generated function's core differ from a natively decoded one's whenever the function has such a literal, and matching a decoded UCB against a generated body by core digest would then miss. The form therefore travels as a second bit map in the identity section, beside the atom map, and only an import acts on it: after the reference-closure checks, each marked butterfly is rebuilt as generation builds it, interning every element through `atomStringToJSStringMap`, since the fast path's absence test reads that map and a structure switched without interning would make `indexOf` return -1 wrongly. A decoded or live UCB keeps the form its native path gave it, which is its native twin's. Twin T6 now covers the forms the core bytes leave out.

### `RegExp` constants by pattern and flags

`CachedRegExp::encode` wrote the cell's atom, specific pattern and parsed bit. A `RegExp` is a cell the VM shares through `RegExpCache`, and `RegExp::deleteCode` clears the atom and specific pattern on every live `RegExp` when `VM::deleteAllCode` runs, as `Bun.shrink()` makes it. A core's encoding then depended on whether code deletion had run since generation, and a JITCache decode could return a live cell in another state, so S1, S2, decoded-UCB matching and T1 failed on legitimate bodies. Codec rule E11 writes only the pattern and the flags in the JITCache mode; decoding then takes `RegExp::create`, the call generation makes. The decode now parses the pattern when no live cell exists, as generation does, and C5 checks the result is valid, since generation emits only valid ones.

### One provenance rule for every core kind

Matching already treated a natively decoded UCB as content no key pins, because nothing records the options an embedder payload's generator ran with. The body side applied the same premise only to programs and modules, so a function body captured from a lazily decoded slot could replace generation, or attach to a generated UCB on key, context and holder alone. With a payload generated under other bytecode-relevant option values, that installed an image on a UCB with another instruction stream when strict was off, and turned the cache off over a third-party difference when it was on. Now a body of provenance `EmbedderDecoded` serves only a natively decoded UCB with its core, for every kind. The rule is smaller than the kind-dependent one, makes the "from the same inputs" claims of sections 6.3.7 and 10.1 true, and makes the capture's mapping of an imported UCB to provenance `Generated` correct, since an import now takes only such bodies. It also makes an import's string constants atom-backed exactly where the producer's were.

One cost remains. When a ConsumerProducer decodes a function natively, seeds it from a `Generated` body with the same core and later commits a richer capture, that capture has provenance `EmbedderDecoded`, and consumers that generate the function miss until a generating producer commits a richer one. Carrying `Generated` forward would need the record to keep the matched body's butterfly map, for a case that needs one artifact shared between a bytecode-compiled build and a source build. The lane accepts the miss.

### THREAD gaps raised

Unchanged from revision 2. Item 6 (atom-ness, section 10.1) narrows: an import now meets "exactly", and only a natively decoded UCB cannot.

## Design record, revision 4 (system review round 1, 2026-10-06)

The system review of the composed design filed no finding against this lane alone. Two of its findings touched the lane's sections.

### The atom-ness guarantee is reconciled

Two majors against the Image lane showed that its twin check failed on correct imports into natively decoded UCBs: the twin compiled against the consumer's atoms, which can include constants the producer's compilation still saw as plain, while the image kept the producer's generic comparisons. The Image lane now records, per strict-equality instruction, which operand its template compared inline, and the twin takes that choice back; its R-UCB-1 asks only that every constant compared inline be an atom here. That is the direction section 10.1 already guaranteed, so the provisional note there, which asked the Image lane and the integrator to reconcile "exactly" with the lane's guarantee, is replaced by a plain statement. Item 6 of the THREAD gaps raised is settled within the lanes: THREAD's index-space guarantee never covered atom-ness, and no THREAD text is needed.

### Exit sites a consumer induced at polymorphic sites (THREAD gap)

A major filed against THREAD showed a path by which an import can lower what later consumers start from. Every native fact checks out. The CB lane's `finishCounter` arms `max(nativeSlice, 2 × executionCounterIncrementForEntry)`, so a non-looping body whose captured progress had crossed calls `operationOptimize` at its second invocation; the ICs lane restores no case; and a DFG compile that parses an IC listing one of a site's shapes speculates with `CheckStructure`, whose exits are `BadCache`. Once the exits jettison the code, `CodeBlock::jettison` calls `tallyFrequentExitSites`, and `OSRExitBase::considerAddingAsFrequentExitSite` adds the site of every exit with a nonzero count to the UCB's exit profile. From then on `hasBadCacheExitSite` makes `GetByStatus::computeFor` return `slowVersion()`, as `PutByStatus`, `InByStatus`, `DeleteByStatus`, `CheckPrivateBrandStatus` and `SetPrivateBrandStatus` do for their own sites. Section 8.6 scores each exit site, THREAD orders richness before the IC count, and so a ConsumerProducer's recapture after such a jettison wins and writes the induced sites into the artifact.

Both levers are THREAD's: its richness rule counts every exit site, and its floor and its omission of IC cases bring the early compile. A lane-local exclusion of exit sites at bytecodes whose captured IC listed several cases would bind the ICs lane, the integrator's scoring and this lane's summary at once. Section 8.6 keeps THREAD's rule and marks the point provisional; SPEC-cb.md marks the floor beside it.

## Design record, revision 5 (system review round 2, 2026-10-06)

The second system review filed two majors against this lane, both about cost, and a third against the CB and Image lanes whose fix changes the twin report this lane's tests read. All three held up against the code.

### Imports are not re-encoded, and digests hash the encoder's pages

S1 re-encoded every imported UCB when strict was on, and matching re-encodes every natively decoded UCB whether or not it is. Each run went through the bytecode cache's `Encoder`: a first page of `encoderMinPageSize << 4`, 64 KiB, zeroed because padding ends up in the payload; pointer and string maps that deduplicate as it writes; and `Encoder::release`, which copies every page into one new buffer before SHA-256 reads it. All of it runs inside `unlinkedCodeBlockFor` or `getUnlinkedGlobalCodeBlock`, before `ScriptExecutable::newCodeBlockFor` creates the CB, so it fell outside THREAD's import window and outside its list of costs measured separately.

S1 is gone. Decoding a core and encoding the result gives the core back unless the codec is wrong: the decode depends only on the validated bytes and the build, E4 and E5 turn what it cannot reproduce into decoder failures, and E10 and E11 keep the VM's mutable state out of the bytes. A wrong codec fails the same bodies in every run, which twin T1 checks on every import of a twins build and U7 over a corpus, so the production check repeated a test at the price of a full encode per import. The reuse check, S2 until now, is renamed S1 and stays: THREAD Identity requires it, and it tests an assumption about generation inputs that a run can falsify.

Matching keeps the codec as the definition of a core's content. The review suggested a hash-only walk over the fields the codec writes, carried as a separate digest in `ucb.identity`. That walk would be a second definition of the content. It would have to track every field the codec writes through each WebKit bump, which the codec's owners do for the bytecode cache and this lane would have to repeat, and a field it missed would let two different UCBs match, a silent failure; the codec's coverage is what the import decodes from. Two codec rules take most of the cost away instead. E12 starts `JITCacheCore` encoders at a 4 KiB page that doubles, where the native first page is 64 KiB, and E13 hashes the pages in place through two chunk entry points, so neither a match nor a holder digest allocates or copies a payload. The deduplicating maps stay, since they shape the bytes. B7 measures what remains, split into encoding and hashing.

### Request-point work and THREAD's bound (THREAD gap)

THREAD counts the seeding done during decoding and nothing else of the request point, which runs before the native link that opens THREAD's window, and its list of costs measured separately names the decode, the key digest and reading the artifact. Matching, the closure checks, the section parses, atomization, parse fields and records fall in neither. Section 14 counts them toward the bound with the seeding, which keeps every cost JITCache adds bounded, and leaves the native decode, the butterfly rebuild that completes it, the key and context digests and `openBody` where THREAD's list puts them. B2 measures each part apart, so the bench can apply another reading. A parent with hundreds of children pays one descriptor encode per child in its core digest. Such parents are mostly module and wrapper top levels, which run once and rarely reach baseline, and B2 shows whether a captured one breaks the bound.

### A root's own singleton bit stays native (THREAD gap)

Section 6.3.6 read a root's own singleton bit from its captured call or construct body when the root was created: `openBody`, the identity parse, a holder digest and the whole feedback validation, for one bit. Builtin generators link at creation (`Scripts/wkbuiltins/builtins_templates.py`), and global object setup calls many of them eagerly through `JSC_BUILTIN_FUNCTION_WITHOUT_TRANSITION`, thirteen in `ArrayPrototype::finishCreation` alone. A consumer's first global object therefore opened and validated every captured builtin body, including builtins that never run, and the bodies that did run were opened, parsed and hashed again at their import.

The bit is learned by the code that creates a root's functions, the `Function` constructor or the engine code that creates builtin functions, when it makes a second function of one executable. THREAD's opening carries "everything else the baseline phase learned". The child bit fits, because the parent's `new_func` creates the closures; the root bit does not, because no body's baseline phase creates them. Its effect is small as well. The only reader of a root executable's singleton is the root's own DFG compile, through `GetCallee` folding in `ByteCodeParser::get` and in the abstract interpreter. An eagerly created builtin's executable makes one function per realm, so its bit is usually clear, and where it is set a consumer without it can jettison one compile. The lane leaves the bit native, and section 6.3.6 records only the root's identity. The feedback section's holder byte had no other use and is now reserved; `verifyRootSeed` and the `rootSeeds` statistic are gone.

The review's first suggestion was to make the read cheap: validate sections on first access, keep the opened body on the registry entry for the first body request, and cache the holder digest per UFE. It would still have opened a file per captured root during global object setup, against THREAD Session's "does no work ahead of demand", and it needed new duties from the integrator. THREAD Storage leaves no cheaper place for the bit: bodies share nothing across files, and the header holds only build, option and CPU facts. The identity digest still runs at creation, because the UFE does not keep the root source it covers. It is THREAD's key digest, which B1 measures, and B8 now measures global object setup against JITCache off.

### The twin report keeps skips apart

The Image lane's twin check now skips itself wherever it cannot be exact, including whenever `useConcurrentJIT` is on (SPEC-image.md, section 17.2). `atom-constants.js` needs that check to run, so its runs turn the option off.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): narrowed. A child's bit travels in its parent's feedback; a root's own bit stays native.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): unchanged.
5. `vm.compileFunction` (section 6.2.6): unchanged.
6. Exit sites a consumer induced at polymorphic sites (section 8.6): unchanged.
7. Request-point work and the installation bound (section 14): new. It counts with the seeding, apart from the native decode, the butterfly rebuild, the key and context digests and `openBody`.

## Design record, revision 6 (system review round 3, 2026-10-06)

The third system review filed a major against this lane and a major against THREAD whose requirement lands here. A blocker against the CB lane touched nothing of this lane. Both majors held up against the code.

### Supplied root digests

The finding: every root's key digest hashed the root's whole transformed source, in every role, on paths where the engine reads no text. Each fact checks out (F25). `bun build --compile` records each module's `StringImpl` hash under `Flags::HAS_SOURCE_HASHES`, whose comment gives the purpose, a launch from bytecode that never pages in its source. `Zig::SourceProvider` returns that hash from `hash()`, `SourceCodeKey` hashes through the provider and compares no text under `USE(BUN_JSC_ADDITIONS)`, and the bytecode is a persistent borrowed payload with embedded `module_info`. Bun's standalone internal modules decode through `decodeBuiltinFunction`, which reads only the provider's length. The fork already moved the builtin metadata scan to build time (`compute_builtin_source_metadata`), so release builds create builtins without reading their text, and global object setup creates many builtins whose bodies never run. Revision 5 had accepted the eager identity digest at root creation as THREAD's key digest, measured by B1 and B8. With the facts above, that cost falls on the startup paths JITCache exists for, in Consumers and Producers alike, against THREAD Session's "Outside recording, capture and first installation, its overhead is near zero".

The revision takes the finding's design. A root's source digest now comes from where the text was produced when that place recorded one (section 4.4): the builtins generator writes each JSC builtin's digest beside the metadata it already computes, and a provider may return one from a new `SourceProvider::jitCacheSourceDigest()`, which Bun fills for the modules `bun build --compile` embeds and for its internal modules (section 6.2.8). Only roots without a supplied digest are hashed, and on those paths the JITCache-off run reads the text as well.

One part goes beyond the finding. A supplied digest is a statement by whoever produced the text, like `hash()` and the bytecode payload, which JSC trusts natively. The builtin metadata's digest belongs to the build and is checked as the codec's round trip is, by U9 and debug assertions. A provider's digest belongs to the embedder's output, and a mistake there would give two texts one key. A natively decoded UCB is matched by its core and S1 checks an attached generated UCB's core, so a wrong key can only make them miss; an import replaces generation on the strength of the key. Strict therefore verifies a provider's digest at the first import that relies on it, once per provider in the VM (S2). The registry keeps a reference to that provider beside every key derived from the root, in VMs that import with strict on, so a child body or a direct eval of the tree is verified at its own import. Reading the provider from the request's own source would not do: a direct eval's text has its own provider, and a nested direct eval leaves no request field that reaches the root's. Verifying at the decode would bring back the cost the finding removes, and verifying at capture would protect nothing the consumer's check does not.

The finding's per-provider cache for computed digests was not adopted. No path computes one provider's digest twice except an attach after released parked code, which section 6.3.4 already limits, and Bun's internal modules make a new provider per global object. B9 takes the suggested startup measurement, with strict on and off and for compiled apps with and without `--bytecode`.

### Plan-site faults (THREAD gap)

A major against THREAD: no part owned the executable-allocation fault for a DFG or FTL plan, though THREAD Failures requires it "before its effects are written". Verified (F24). DFG and FTL plans that cannot allocate install a `FailedFinalizer` on the compiling thread, the failure surfaces in `DFG::Plan::finalize`, and the callbacks then defer the baseline counter (the CB lane's P12) and clear the quick DFG or FTL bit; a failed baseline plan defers this lane's LLInt counter in `BaselineJITPlan::finalize`. Three of those writes are this lane's state. R-INT-11 asks the integrator, which THREAD gives "the fault plumbing", to raise the fault in the baseline plan's failure case and in `DFG::Plan::finalize` before the callback. The DFG plan carries a cause flag, set where the compiling thread sees `didFailToAllocate` or `allocationFailed`, because `FailedFinalizer` records none and the plans that fail for other reasons must keep their effects, which THREAD carries. The reading is provisional. The Image and ICs lanes keep the sites they already edit, and the remaining `JITCompilationCanFail` sites (Yarr, Bun's FFI thunks and IC stubs, WebAssembly), which write nothing a capture reads, stay native under it.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): unchanged.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): unchanged.
5. `vm.compileFunction` (section 6.2.6): unchanged.
6. Exit sites a consumer induced at polymorphic sites (section 8.6): unchanged.
7. Request-point work and the installation bound (section 14): unchanged; S2's verification counts with the key digest.
8. Plan-site faults (R-INT-11): new. The integrator raises the fault at every tier's plan site, before the failure's effects.

## Design record, revision 7 (five-part system review round 1, 2026-10-06)

### Induced exit sites across generations (THREAD gap, unchanged)

The first review of all five SPECs together filed the THREAD gap of revision 4 again, adding that nothing measured more than one consumer generation. The chain still holds in the code, and the sites accumulate: `DFG::ExitProfile::add` only appends, and section 8.3 seeds the captured list into an imported UCB through `restoreFrequentExitSites`, so a recapture of an imported body carries every saved site plus the ones its own process induced. Section 8.6 keeps THREAD's rule, one unit per exit site, and its provisional mark now says that a generation can only add such sites and points to SPEC-integrator.md IB10, which chains ConsumerProducer generations and counts, per generation, the sites added at bytecodes whose captured IC listed two or more cases.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): unchanged.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): unchanged.
5. `vm.compileFunction` (section 6.2.6): unchanged.
6. Exit sites a consumer induced at polymorphic sites (section 8.6): unchanged as a choice; the integrator's IB10 now measures it across generations.
7. Request-point work and the installation bound (section 14): unchanged.
8. Plan-site faults (R-INT-11): unchanged.

## Design record, revision 8 (five-part system review round 2, 2026-10-06)

### Contexts are digested where they are read

Every request computed its context together with its key, before `bodyVersion` said whether the artifact held a body, and every record stored the digest, in every role. Most contexts cost one SHA-256 of a few dozen bytes, but two cost far more. A root UFE body's context holds `holderDigest(ufe)`, a whole JITCacheCore descriptor encode: an `Encoder` that starts with a zeroed 4 KiB page, its string and pointer maps and `encodeDeferred` (the `Encoder` constructor and `allocateNewPage` in `runtime/CachedTypes.cpp`, codec rules E12 and E13). A direct eval's context sorts every TDZ and private name with `codePointCompare`, and `JSScope::collectClosureVariablesUnderTDZ` (`runtime/JSScope.cpp`) adds every symbol-table entry of every lexical and catch scope on the chain, a module's import entries included; `JSC::eval` (`interpreter/Interpreter.cpp`) collects them at every `DirectEvalCodeCache` miss and only inserts them into a hash set. So the first call of every builtin body and every `new Function` body, and every such direct eval, paid an encoder run or a sort of thousands of names, usually for a key with no body, which THREAD Session's "does no work ahead of demand" and "Outside recording, capture and first installation, its overhead is near zero" exclude. A root body's import also digested its holder twice, in the context and again for C11.

Only matches and captures read a context digest: an import, a seeding or an attach against a body at the key compares it, and a capture writes it. A request now computes its context at the first comparison with a body, after the key comparison and, at an import, after the provenance check, and keeps it and the holder digest in its state, so C11 reuses the digest a root's context took. A record keeps the context's inputs instead (`RecordedContext`): the offset and first line, and a global's four other fields, all fixed at the request. An attach or a capture computes the digest from them, for a root UFE body with that UFE's holder digest, which does not change while the UFE lives: codec rules E1 and E2 leave out its slots and the fields its own parse sets, and the code that creates a UFE (`UnlinkedFunctionExecutable::create`, then `setEcmaName`, `setClassSource` and `setClassElementDefinitions` in `BytecodeGenerator`, and the directives in `CodeCache::getUnlinkedGlobalFunctionExecutable`) sets every other field before the UFE is published. C11 already relied on that, and U3 tests it. Neither reader lacks the UFE: an attach comes from a request on it, and a capture reaches it through the CB's `FunctionExecutable::unlinkedExecutable()`.

A direct eval's TDZ and private-name sets live on `JSC::eval`'s stack until `DirectEvalExecutable::create` returns, and generation reads them through `const` pointers. A copy per record would keep every binding name of the scope chain for each eval UCB of a producing VM. The record keeps the digest instead: the one its import computed, or, in a VM whose production is active, one `recordedContext` computes while the sets still exist. No request attaches to a direct eval, and a VM without active production never captures, so there the digest would serve nothing. The lane asks the integrator for `productionActive()`, which its `VMState` already had (R-INT-1), and B3 measures the remaining cost per direct eval against the native miss.

Two counters, `contextDigests` and `holderDigests`, make the rule observable: U8 checks the engine's paths, and `context-on-demand.js` checks it end to end in a Consumer and a ConsumerProducer.

The review also suggested build-time holder digests for JSC builtins, as form 1 of section 4.4 does for their source digests. Not adopted. After this revision a builtin's holder digest runs only for a body the artifact holds, beside an import or attach that costs far more, or at a capture. The descriptor is the codec's encoding of the UFE, which only the engine runs; the builtins generator is Python and would need a second definition of the descriptor, the objection section 18 already makes to a hash-only walk of the core. Its other suggestion, skipping C11 for a root whose context matched, is unnecessary once the request keeps the holder digest: C11 is then one comparison of 32 bytes, and it stays the same check for every function body.

### Task plan

Section 17 said the self-tests run once tasks 1 to 9 have landed. They run through `$vm.jitCacheUCBSelfTest()`, which the integrator's task 12 adds after this lane's tasks 6 and 10, so they run once both have landed; the twins run from task 10 on, and the JS tests need the integrator's runner. No task of this lane waits on an integrator task that waits on it.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): unchanged.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): unchanged.
5. `vm.compileFunction` (section 6.2.6): unchanged.
6. Exit sites a consumer induced at polymorphic sites (section 8.6): unchanged.
7. Request-point work and the installation bound (section 14): unchanged.
8. Plan-site faults (R-INT-11): unchanged.

## Design record, revision 9 (five-part system review round 3, 2026-10-06)

### A body's cost stops growing with its enclosing scopes

The finding: every function of a module holds a TDZ chain ending in a link that names each lexical binding of the module, and the lane wrote that chain whole wherever it encoded a UFE. The holder digest, run at every function import, at every attach to a generated or imported UCB and at every capture, encoded the enclosing environments in a fresh encoder, sorted their names with `codePointCompare` and hashed them. Every function core whose generation created closures repeated those names, so a capture encoded and hashed them, a seeded decode and S1 encoded them again for `coreDigestOf`, and an import decoded them, sorted them by address and interned them, only to delete the duplicate. Each step checks out in the code (F26): the module generator calls `pushTDZVariables` before its `makeFunction` loop, `getVariablesUnderTDZ` caches one link per TDZ stack entry and parents it on `m_cachedParentTDZ`, `generateUnlinkedFunctionCodeBlock` passes the UFE's chain as that parent, `CachedFunctionExecutable::encode` writes the chain, `CachedTDZEnvironmentLink::encode` recurses on the parent, `CachedCompactTDZEnvironment::encode` sorts with `EncodingOrder`, and `CachedCompactTDZEnvironmentMapHandle::decode` interns a fresh environment. One detail the finding leaves implicit: `CachedPtr::encode` deduplicates a link or an environment by address within one encoder, so a core holds each enclosing environment once, however many of its children reach it. Across bodies the cost still grows with functions times bindings, and per body with the bindings, so the finding holds. Section 14 counts C11's holder digest and the decoded core digest toward THREAD's bound, so a small function at the top level of a large bundle would have failed it.

The revision adopts both parts of the finding's design, in the codec's terms. Codec rule E14 writes a function core's TDZ chains relative to its holder's: a chain equal to the holder's, link by link, becomes one start record, and the decode returns the requesting UFE's chain for it. Equality is by environment object, which the VM interns by content, rather than by link: a natively decoded UCB's links come from a payload, and comparing environments makes it cut its chains where a generated UCB cuts them, so I14 and the match of a decoded UCB against a generated body still hold. Decoded children now share the requesting UFE's chain object, as generated children share theirs, and T5 checks it.

The holder digest takes the chain out of the descriptor the same way, with the UFE's own chain as the start chain, and appends a chain digest: per link, a hash of its environment's digest and of its parent's digest. The environment's digest is kept in the environment, in a new member of `CompactTDZEnvironment` that is freed with the names. A map in the registry, keyed by the environment's address, would have to pin each environment it held, which keeps a scope's names alive after the code that declared them died, or else hook `CompactTDZEnvironmentMap::Handle::~Handle`, which cannot reach the VM. The member costs one pointer per interned environment in every VM, with JITCache on or off. Links are not memoized: a chain has one link per enclosing lexical scope that declares something, and each link costs one hash of 80 bytes. With both changes, C11 hashes the UFE's own fields and one record per link, a function core holds only its own generation's links, and each environment is sorted and hashed once while it lives (I25). B10 measures it and `tdz-scale.mjs` tests it.

The finding's first fix proposed a marker for "the holder UFE's chain" written after the links the UCB's generation created. E14's start record serves as that marker, and the encoder places it wherever a chain equals the start chain, which spares the encoder from knowing which links its own generation created. Its second fix proposed memoizing "by the shared environment or link"; only environments are memoized, for the reasons above. Its remark that a producer sorts and hashes a direct eval's sets again for each eval's context was refuted (SPEC Notes): `JSC::eval` collects the set at each `DirectEvalCodeCache` miss and the eval's generation interns it, sorting it by address, so the native request already sorts the same names, and the lane digests the context only when an import found a body or a producing VM records the UCB.

The kept environment digest is charged to no production limit. THREAD exempts only the registry by name, so section 4.3 marks this reading provisional: the digest serves C11 and root contexts in every role, as the registry does, and charging it would need a release when the environment dies, a path from which no budget is reachable. A capture still charges the sort buffer of each environment it digests first. Section 14's provisional accounting now counts each environment's first digest with the key digest.

### THREAD gaps raised

1. Parse-time stack overflow (section 6.3.7): unchanged.
2. Owner of UFE-held learned state (section 8.1): unchanged.
3. Decode points (section 6.3.3): unchanged.
4. Decoded roots' keys (section 6.3.3): unchanged.
5. `vm.compileFunction` (section 6.2.6): unchanged.
6. Exit sites a consumer induced at polymorphic sites (section 8.6): unchanged.
7. Request-point work and the installation bound (section 14): extended. Each TDZ environment's first digest counts with the key digest.
8. Plan-site faults (R-INT-11): unchanged.
9. The kept TDZ environment digest (section 4.3): new. It is charged to no production limit, as the registry is not.

## Review records

### Five-part system review round 3 (2026-10-06)

One major was filed against this lane, with two majors against the integrator and the Image lane that do not touch it. The one against this lane was verified against the code and accepted.

- Major, holder digests and per-body cores re-encode the enclosing scopes' TDZ environments: confirmed in `BytecodeGenerator::BytecodeGenerator` for `ModuleProgramNode`, `pushTDZVariables`, `getVariablesUnderTDZ`, `generateUnlinkedFunctionCodeBlock`, `CachedFunctionExecutable::encode`, `CachedTDZEnvironmentLink::encode`, `CachedCompactTDZEnvironment::encode`, `EncodingOrder::sort`, `CachedCompactTDZEnvironmentMapHandle::decode` and `CachedPtr::encode`. Fixed with codec rule E14 and a TDZ chain digest whose environment digests are kept in the environments (F26; sections 3, 4.3, 4.5, 5.6, 6.3.1, 6.3.2, 6.3.3, 6.3.4, 6.6, 6.7, 7.3, 7.5, 9.2, 13.1 to 13.3, 14, 16, 17 and 18; codec sections 1 to 4, 6 and 7). Its direct-eval remark was refuted (Notes), and the charging of the kept digest is provisional.

### Five-part system review round 2 (2026-10-06)

One major was filed against this lane; two against the CB and ICs lanes concerned the task plans the integrator shares with it. All three were verified against the SPECs and the code and accepted.

- Major, contexts digested before the body lookup and recorded for every generated UCB in every role: confirmed in sections 6.3.1, 6.3.3 and 6.3.5, in `JSScope::collectClosureVariablesUnderTDZ` and `JSC::eval`, and in the `Encoder` constructor and `allocateNewPage`. Fixed as revision 8 records (sections 1, 4.3, 4.5, 5.1, 5.6, 6.1, 6.2.3, 6.2.6, 6.3.1 to 6.3.5, 6.3.7, 6.6, 6.7, 7.4, 9.1, 9.2, 10.2, 13.1, 13.3, 14, 16 and 18). The build-time holder digests it suggested for builtins were not adopted (Notes).
- Majors against SPEC-cb.md and SPEC-ics.md, twin checks bundled with tests that need the integrator's glue: settled there and in SPEC-integrator.md section 18. Section 17's account of when this lane's self-tests run was incomplete and is corrected (revision 8).

### Five-part system review round 1 (2026-10-06)

No finding was filed against this lane. Two touched it:

- Major against THREAD, exit sites induced across ConsumerProducer generations: confirmed in `CodeBlock::jettison`, `CodeBlock::tallyFrequentExitSites`, `OSRExitBase::considerAddingAsFrequentExitSite`, `DFG::ExitProfile::add` and `GetByStatus::computeFor`. Still a THREAD gap; section 8.6's mark points to the integrator's new IB10 (revision 7).
- Major against the integrator, the twins build's heap domain: settled there; this lane's twin checks hold no address.

### System review round 3 (2026-10-06)

One major against this lane, one major against THREAD and one blocker against SPEC-cb.md. All three were verified against the code and accepted.

- Major, root key digests read whole sources that Bun's bytecode path and JSC's builtin path never read: confirmed in `Zig::SourceProvider::hash` and the provider creation in `ZigSourceProvider.cpp`, `Flags::HAS_SOURCE_HASHES` and `File::to_wtf_string` in `StandaloneModuleGraph.rs`, the standalone `ResolvedSource` in `jsc_hooks.rs`, `UnlinkedSourceCode::hash`, `SourceCodeKey`, `generateInternalModule`, `decodeBuiltinFunction` and `BuiltinExecutables::createExecutable`. Fixed with supplied digests and S2 (F25; sections 3, 4.2, 4.4, 4.5, 5.1, 5.3, 5.6, 6.1, 6.2.3, 6.2.5, 6.2.8, 6.3.1, 6.3.3, 6.3.5, 6.3.6, 6.6, 6.7, 10.2, 11, 13.1 to 13.3, 14, 15, 16 and 17).
- Major against THREAD, plan-site faults: confirmed in `SpeculativeJIT::compile`, `DFG::Plan::compileInThreadImpl`, `FTL::compile`, `DFG::Plan::finalize`, `CodeBlock::setOptimizationThresholdBasedOnCompilationResult`, `didFailDFGCompilation`, `DFG::JITCode::setOptimizationThresholdBasedOnCompilationResult`, `didFailFTLCompilation` and `BaselineJITPlan::finalize`. A THREAD gap: R-INT-11, provisional (F24; sections 8.1 and 11.1).
- Blocker against SPEC-cb.md, metadata walks on a null table: settled there. This lane walks no CodeBlock metadata.

### System review round 2 (2026-10-06)

Two majors were filed against this lane and one against the CB and Image lanes. All three were verified against the code and accepted; the two against this lane each leave a point only THREAD can settle.

- Major, imports and seeded decodes re-encode the whole core outside the installation bound: confirmed in `Encoder::allocateNewPage` (a zeroed first page of `encoderMinPageSize << 4`), `Encoder::release` (every page copied into one buffer) and `ScriptExecutable::newCodeBlockFor` (`unlinkedCodeBlockFor` before `FunctionCodeBlock::create`). S1 removed and S2 renamed S1 (sections 6.3.1, 11.1, 11.2, I7); codec rules E12 and E13 with their chunk entry points, and `coreDigestOf` (sections 4.5, 6.3.2, 6.6, 7.5); B2, B7 and a provisional accounting (section 14). The suggested hash-only walk was not adopted (Notes).
- Major, the root read opens captured builtin bodies during global object setup: confirmed in `builtins_templates.py`, `JSC_BUILTIN_FUNCTION_WITHOUT_TRANSITION` and `ArrayPrototype::finishCreation`, and the bit's readers in `UnlinkedFunctionExecutable::link`, `ByteCodeParser::get` and the `GetCallee` case of `AbstractInterpreter`. The root bit stays native, marked provisional (sections 5.4, 5.6, 6.2.5, 6.3.6, 6.6, 7.1, 8.1 to 8.3, 8.7, 9.2, 10.2, 11.1, 13.2, 13.3, B8, I5, I10). The suggested lazy validation was not needed (Notes).
- Major against SPEC-cb.md and SPEC-image.md, twin checks under concurrent compiles: settled there. `atom-constants.js` now runs with `useConcurrentJIT` off, so the Image lane's check runs.

### System review round 1 (2026-10-06)

No finding was filed against this lane. Two touched it:

- Majors against SPEC-image.md, twin compile and atom-ness: confirmed in `JIT::compileOpStrictEq`, `JIT::compileOpStrictEqJump`, `CachedJSValue::decode` and `DecoderStringTable::atomFor`. Settled in the Image lane; section 10.1's provisional note is replaced by a statement of the reconciled guarantee.
- Major against THREAD, induced exit sites outscoring the producer's capture: confirmed in `GetByStatus::computeFor`, `hasBadCacheExitSite`, `CodeBlock::tallyFrequentExitSites` and `OSRExitBase::considerAddingAsFrequentExitSite`. A THREAD gap: section 8.6 counts every exit site, marked provisional.

### Round 3 (2026-10-05)

All four blocker and major findings were verified against the code and accepted; one part of one suggested fix was refuted.

- Blocker, contexts omit the source's start line: confirmed in `Lexer::setCode`, `Parser::parseClass`, `ASTBuilder::createClassExpr`, `CachedFunctionExecutableRareData::packClassSource`, `CachedJSTextPosition::encode`, `Parser::parseClassFieldInitializerSourceElements`, `BytecodeGenerator::emitNewClassFieldInitializerFunction`, `UnlinkedFunctionExecutable::linkedSourceCode` and `BytecodeGenerator::emitExpressionInfo`. Fixed with a `firstLine` field in every context (sections 4.3, 4.5, 6.2.3, 6.3.5, 6.3.6), F21, I23, U3 and `start-line.js`. The suggested start column was refuted: no UCB depends on it (F21; SPEC Notes).
- Major, imports decode all-string literals without the atom-strings butterfly: confirmed in `ArrayNode::emitBytecode`, `CachedImmutableButterfly::decode`, `arrayProtoFuncIndexOf`, `operationCopyOnWriteArrayIndexOfString` and `compileArrayIndexOfOrArrayIncludes` in `SpeculativeJIT` and `FTL::LowerDFGToB3`; the runtime `includes` has no such path. Fixed as suggested, with the butterfly map in `ucb.identity`, import step 9, C8, E10, T6, I22, U5 and `array-literals.js`.
- Major, `RegExp` constants make the core depend on the VM's shared state: confirmed in `CachedRegExp::encode`, `RegExp::deleteCode`, `RegExpCache::deleteAllCode`, `VM::deleteAllCode`, `VM::shrinkFootprintWhenIdle` and Bun's `shrink` and `JSC__VM__shrinkFootprint`. Fixed as suggested with codec rule E11, F22, C5's validity check, I13, I14, U7 and `regexp-constants.js`.
- Major, provenance gates only program and module bodies: confirmed in the SPEC's own steps and in `GenericCacheEntry::isUpToDate`, `BytecodeGenerator`'s `Options::useTailCalls` read and `NodesCodegen`'s `Options::switchJumpTableAmountThreshold` read. Fixed as suggested (sections 6.3.1, 6.3.2, 6.3.7, 7.2, 9.2, 10.1, 11.1, I7, U8 and `context-miss.js`).

### Round 2 (2026-10-05)

All nine blocker and major findings were verified against the code and accepted; none was refuted.

- Blocker, global and direct-eval keys read features the native path overwrites: confirmed in `ProgramExecutable::ProgramExecutable`, `Interpreter::executeProgram`, `ScriptExecutable::recordParse`, `GlobalExecutable::recordParse`, `generateUnlinkedCodeBlockImpl`, `recordParseFromUnlinkedCodeBlock` and the parser's root scope. Fixed by the snapshot (sections 6.1, 4.2), I20, U3 and `role-keys.js`.
- Blocker and two majors, recording the other decoded slot settles the request: confirmed in `generateUnlinkedCodeBlockForFunctions` and `decodeCachedCodeBlocks`. Fixed in sections 6.1, 6.2.2 and 6.3.5, I19, U8 and `decoded-slots.js`.
- Blocker, provenance in UFE-body contexts makes `vm.compileFunction`'s function miss: confirmed, and C8 would still have missed for most decoded functions, as the finding says. The suggested provenance field and atom swap were adopted; atom-ness also left the core bytes, so the digest no longer depends on decode history, and the provenance check went to program and module bodies only (sections 4.3, 6.3.2, 6.3.3, 7.2, 7.4; codec E10).
- Major, `count() == P` claimed but the arming adds the truncated fraction: confirmed in `ExecutionCounter::setThreshold`, `CodeBlock::jitSoon` and `applyMemoryUsageHeuristics`. Fixed with the integral slice the finding suggested (section 8.5), I12, U6 and `llint-counter.js`.
- Major, key and context do not pin the embedder payload: confirmed in Bun's `constructAnonymousFunction`, `getBytecode` and the `setGlobalScopeExtension` call, and in `decodeCodeBlockImpl`. Fixed as the finding suggested, with the decoded UCB's own with-scope bit in its key and an always-on core digest that misses; C11 was added for the generation inputs a decoded parent's key still leaves open.
- Major, THREAD's `vm.compileFunction` import stated unconditionally: confirmed. Marked Provisional in section 6.2.6; `compile-function.js` now runs both flows.
- Major, attach hashes the whole source on every CodeCache hit: confirmed. Fixed in section 6.3.4 with the record's key bits, the matched version and the order of the checks, and B3, I21 and U8 now cover CodeCache hits.

### Round 1 (2026-10-05)

All nine blocker and major findings were verified against the code and accepted; none was refuted.

- Blocker, SymbolTable singleton state dropped: confirmed in `CodeBlock::finishCreation`, `ModuleProgramExecutable::getUnlinkedCodeBlock`, `SymbolTable::notifyCreation`, `SymbolTable::cloneScopePart` and `CachedSymbolTable::encode`. Fixed with constant bits in `ucb.feedback`, seeding step 8, check C9 and twin T4. The root UFE part was confirmed in `FunctionExecutable::fromGlobalCode` and the builtin generator templates, and fixed by reading the root's own bit at its creation (section 6.3.6).
- Major, lazily decoded slots never seeded: confirmed (F1). Fixed by seeding the decoded UCB before `m_isCached` clears (section 6.3.3), and the same at the CodeCache's provider decode; the old Provisional on `vm.compileFunction` is gone.
- Major, three findings on `DirectEvalExecutable::create`'s second caller: confirmed in `DebuggerCallFrame::evaluateWithScopeExtension`. Fixed with a defaulted site pointer (section 6.2.4).
- Major, interfaces and task dependencies not frozen: fixed with sections 6.1, 6.7, 7.5, 8.7, 13.2 and task 0.
- Major, `UCBStatistics` without owner or read path: fixed in section 5.6 and manifest M4.
- Major, twin verification with two owners: the engine calls the verify functions; R-INT-10 now asks only for the sink and the runner.
- Major, `attachMissed` against `live-attach.js` and THREAD's reuse rule: fixed with version-stamped misses (section 6.3.2) and a rewritten test.

## Drain after the thread-prep run (2026-10-06)

THREAD changed after the thread-prep run. The drain resolved the set's nine Provisional marks against the new text, applied the rest of the change and settled the five open blocker and major findings filed against this lane.

Seven marks stand as the lane had chosen them and are now plain statements:

- the decode points keep the native decode and seed its result (revision 1);
- a root UFE's own singleton bit stays local (revision 5);
- an import skips a parse that would overflow the stack, as a CodeCache hit does;
- the kept TDZ environment digests are charged to no limit (revision 9);
- the plan-site faults go through the integrator's `didFailExecutableAllocation` at the sites THREAD Execution names, and every other `JITCompilationCanFail` site stays native;
- every exit site counts toward richness, which is now safe because a body with a polymorphic property IC carries no baseline counter progress, so it never gets the early compile that induced such sites;
- the request point's work counts toward the installation bound, except what THREAD measures apart. The butterfly rebuild moved into the bound, since it is JITCache work rather than decoding, and strict's checks stay out of a measurement that runs with strict off.

THREAD settled the other two differently. A natively decoded root whose with-scope bit differs from its request's gets no key, so it has no record, its children have no identity, and nothing in it imports or is captured. Bun's `vm.Script` and `vm.compileFunction` with `cachedData` stay out of JITCache: the lane no longer edits `NodeVM.cpp`, `didDecodeEmbedderProgram` is gone with the lane headers Bun included for it, and `vm.compileFunction` without `cachedData`, or with data its decode rejects, imports at the program request point.

Strict is now off by default, and normal mode checks only the artifact's integrity. The section rules of 7.2 and 8.2 and checks C1 to C9 became strict checks, listed as SV1 to SV3 in section 11.2 beside S1 and S2, which already were. The parse functions take the strict flag, and with it off they read fields where the layouts put them. What chooses a body stays in both modes: key, context, provenance, C10, C11 and a decoded UCB's core digest. C6, the decode's own report, runs at every import, because the codec's root, symbol, layout and start-record checks are part of decoding. Codec rule E15 defines the validating decode strict runs: placement of every nested record, tags and enums in range, every condition the native decode asserts about payload data checked before the assertion, string hashes and ordinals, and an instruction stream that walks whole from start to end. The CB and Image lanes' strict checks walk the stream, so E15's framing walk is what keeps them inside it.

The blocker against keying decoded roots by their payload's with-scope bit held. THREAD's new rule fixed it, where the review had suggested keying such a root under its request's bits: THREAD gives that root no key, which is also simpler, since it needs no record, no child identities and no core digest. The major on the decoder's shallow checks held for nested records and was fixed with E15. Its suggestion to judge instruction operands was not taken. JSC has no validator for operands, jump and handler targets or the counts that size a UCB's tables; the container's checksum and the header's build ID pin those bytes; the encoder copies the stream verbatim, so a writer or codec bug shows as broken structure or framing, which E15 catches; and an artifact rewritten so that its operands are wrong could carry wrong machine code as easily, which no lane can check, the case THREAD Session's trust rule covers.

The major on version stamps held: a stamp from the opened file's commit identifier was compared with the index's version, which a lagging index never matched. R-INT-3 now has `bodyVersion` answer a token from the index alone, with no envelope read and no filesystem call. Stamps and skips compare tokens, the matched-file shortcut compares commit identifiers, and an open that finds the file gone stamps the token, so a body `compact` removed costs one failed open per UCB until the index forgets it. The integrator's lookup, which reads a body's envelope at its key's first lookup, has to change to meet this.

The major on S2 hashing a whole bundle at one child import held only in part. Strict is off by default, so a default start never runs S2. A direct eval needs no verification at all: its key ends in the digest of its own text, which its request computes, and its generation reads that text and its context, so a wrong supplied digest upstream can change the caller key it is found under, never what generation makes. S2 no longer runs for direct evals, and their records keep no provider. Per-body text digests for roots and function bodies were not adopted: they would add a field to `ucb.identity`, read source text at capture and hash nested ranges once per nesting level, all to make an opt-in, slow mode cheaper.

The major on the freed `cachedData` span was refuted for this lane. THREAD keeps that decode out, the lane neither edits it nor reads what it yields (the user function's request finds no identity and stops), and the use-after-free happens with JITCache off as well, so it belongs to Bun; `compile-function.js` calls the user function only in release runs, which ASan does not instrument.

One defect outside the findings surfaced while verifying section 6.2.8. The internal modules' digests were to grow each record of the builtins section and bump its format version, but `src/exe_format/builtins.rs` reads that section from another platform's executable with a fixed record size and outside the lane's paths. The digests now sit in a table that the header's first reserved word locates, which leaves the records, the version and that reader as they are.

## Kept minors from the thread-prep run

- ucb mismatch R-INT-4 (container guarantees all three sections): fixed, normal mode trusts their presence and strict's `container.required` rejects a body without one, as ICs R-INT-4 says of its section.
- ucb batch 1.1 I16 and E8 understate uncharged encoder allocations: fixed, E8 charges the encoder's tables and per-record scratch, and I16 and B6 state the real overshoot (the rest of one encode, pages doubling to 64 MiB).
- ucb batch 1.2 encoder tables and exit-site copy never charged: fixed, tables and scratch through E8; the exit-site copy is gone, since `forEachFrequentExitSite` visits the sites in place.
- ucb batch 1.3 one-page overshoot bound false: fixed, same change; no early exit was added, since the encode cannot unwind and production ends at the refusal anyway.
- ucb batch 1.4 accounting weaker than I16 (Shape copies, packedTail): fixed, both are scratch E8 now charges.
- ucb batch 1.5 refused charge overshoots far more than one page: fixed, same change; charging an estimate before the encode was not adopted, since no count bounds the encode without running it.
- ucb batch 1.6 one-page bound false (pass 3): fixed, same change.
- ucb batch 1.7 overshoot (pass 4, scratch-page idea): fixed, same change; a reused scratch page would break the offsets later records and `release` read.
- ucb batch 1.8 encoder memory only partly charged: fixed, same change.
- ucb batch 1.9 `UCBSections` holds no budget: fixed, it holds a `RefPtr<ProducerBudget>`, has a constructor and a move constructor that takes the charge, and releases it when destroyed.
- ucb batch 1.10 task 0 headers not implementable: fixed, the lane's functions that read a UCB through its non-const accessors or lock take `UnlinkedCodeBlock&` (section 8.4 states the rule), `UCBSections` holds its budget, and task 0 writes the new `Decoder` members into `runtime/CachedTypes.h`.
- ucb batch 2.1 registry entry per child UFE costs about as much as the UFE: fixed, the children of a recorded UCB share one `ParentIdentity` (key and supplied-digest provider), so a child entry is a reference, its table and its index, 16 bytes; B5 reports the per-UFE figure beside `sizeof(UnlinkedFunctionExecutable)`. The record's optional versions stay, since they cost bytes per UCB, not per UFE.
- ucb batch 2.2 parent key copied into every child entry: fixed, same change; reserving the map from the table counts was not adopted, since a growing WTF map cannot reserve and its growth is amortized.
- ucb batch 2.3 parent key copied into every child entry (pass 3): fixed, same change.
- ucb batch 2.4 key and provider reference per child: fixed, same change; the node holds the provider, and the lock rules now cover dropping a node.
- ucb batch 2.5 child entry costs more than the UFE cell: fixed, same change.
- ucb batch 2.6 registry doubles per-UFE memory: fixed, same change.
- ucb batch 2.7 child entry copies the parent key (pass 4): fixed, same change; a per-parent child vector was not adopted, since `identityOf` must find a UFE's identity from the UFE alone.
- ucb batch 2.8 first import opens its body file twice: already fixed, `bodyVersion` now answers from the integrator's in-memory index with no filesystem call (R-INT-3; SPEC-integrator.md section 7.2, II8).
- ucb batch 2.9 import and seeded decode open the file twice: already fixed, same change.
- ucb batch 2.10 first import reads the file twice: already fixed, same change.
- ucb batch 3.1 const UCB signatures call non-const accessors (pass 1): already fixed by batch 1.10. Every lane function that reads through the non-const accessors takes `UnlinkedCodeBlock&` (section 8.4). The const-reference ones left key the registry by pointer or encode through the codec, whose native `encode` takes `const UnlinkedCodeBlock&`.
- ucb batch 3.2 the same const point (pass 2): already fixed, same change.
- ucb batch 3.3 the same const point (pass 3): already fixed, same change.
- ucb batch 3.4 the same const point (pass 4, native): already fixed, same change.
- ucb batch 3.5 the same const point (pass 4, implementability): already fixed, same change.
- ucb batch 3.6 declared types cannot be built: fixed.
  - The `UCBSections` budget part was already fixed by batch 1.9, and the `ValidatedBody`, `BodyLookup` and twin report part is settled by the integrator.
  - `UCBRegistry` declares its default constructor, because `WTF_MAKE_NONCOPYABLE`'s deleted copy constructor suppresses the implicit one, and the integrator holds it by value.
  - `PendingImport` and `ParentIdentity` declare the private constructors their `create` needs, and `PendingImport` declares its members and an out-of-line destructor.
- ucb batch 3.7 `jsCast` does not exist in this pin: fixed, section 9.2 step 3 uses `uncheckedDowncast<FunctionExecutable>`.
- ucb batch 3.8 task 4 omits headers, `live-attach.js` uses `runInThisContext`, and the twin's direct-eval flags: fixed.
  - The header part was already fixed: task 0 writes the codec's `Decoder` members into `runtime/CachedTypes.h`.
  - `live-attach.js` loops the jsc shell's `loadString`, which evaluates in the current global object, or `vm.runInThisContext` under Bun.
  - The global twin reads `needsClassFieldInitializer`, `privateBrandRequirement` and `isInsideOrdinaryFunction` from `RequestState::globalExecutable`, as `generateUnlinkedCodeBlockImpl` does.
- ucb batch 3.9 `verifyRegistry` flags cells dead but not yet swept (pass 3): fixed.
  - `verifyRegistry` first runs `collectNow(Sync, CollectionScope::Full)`, which sweeps synchronously and asserts nothing is left unswept.
  - `$vm.jitCacheUCBStatistics()` runs the check only when called with `{ verifyRegistry: true }`, so reading statistics mid-test starts no collection.
  - I3 now says no entry outlives its cell's destruction.
- ucb batch 3.10 the same `verifyRegistry` point (pass 4): fixed, same change.
- ucb batch 4.1 seeding's step on a miss contradicts itself: fixed. The contradictory "Stop: nothing recorded" had already gone from section 6.3.3, which left the stop unstated; step 3 now says a match that stops at `Unusable` or invalid material records nothing, since cache activity is off (SPEC-integrator.md section 5.2).
- ucb batch 4.2 same step: fixed, as 4.1.
- ucb batch 4.3 same step: fixed, as 4.1.
- ucb batch 4.4 `atomizeStringConstant` leaves `isDefinitelyAtom` clear: fixed. Checked in `JSString::toAtomString`, `existingAtomOrNull` and `swapToAtomString`: native atomization always marks the cell, and `speculationFromCell` reads the mark. The function now follows `toAtomString`, `existingAtomOrNull()` or `AtomStringImpl::add` then an unconditional `swapToAtomString`; F19 states the mark.
- ucb batch 4.5 seeding's atomization can reach cells published UCBs share: fixed. Checked in `CachedJSValue::decode` and `DecoderStringTable::jsStringFor`: an external-string constant is one VM-wide cell. Section 6.3.3 step 5 now rests the change on `swapToAtomString`'s concurrent-reader contract instead of on `U` being unpublished.
- ucb batch 4.6 the two points together, plus I18: fixed, as 4.4 and 4.5; I18 now says shared cells of other UCBs become atoms too, as a property-key use makes them natively.
- ucb batch 4.7 an attach to a live decoded UCB usually misses `AtomMap`, and decoded-slots.js expected it to attach: fixed in the test. THREAD keeps the miss, so the row now expects an attach only when every marked constant decoded as an atom, and otherwise a miss without a stamp and an attach once a property-key use made the constant an atom. Atomizing at the attach was rejected, as the triage noted.
- ucb batch 4.8 the `AtomMap` miss is stamped although atom-ness only grows: fixed. An `AtomMap` miss is no longer stamped; it sets `matchedBodyVersion`, through a new `setMatchedBodyVersion`, and section 6.3.4 step 5 repeats only C10 against the same file, so no digest is recomputed (I21, U4, U8 and the outcome row updated). Dropping C10 was rejected: THREAD keeps the miss.
- ucb batch 4.9 UCB predictions bounded by `SpecFullTop` while the CB lane uses `SpecBytecodeTop`: fixed. Checked in `ValueProfileBase::computeUpdatedPrediction` and `SpeculatedType.h`: profiles merge only `speculationFromValueForProfiling` of boxed values, so neither `SpecInt52Any` nor `SpecDoubleImpureNaN` can appear. Section 8.2 now bounds them by `SpecBytecodeTop`, as SPEC-cb.md V6 does.
- ucb batch 4.10 same prediction-domain point: fixed, as 4.9.
- ucb batch 5.0 heap-oracle test conventions (cross-set, from SPEC-ics.md section 11.2): fixed. Section 13's intro now says the oracle compares every JITCache run, producers included, in output and, for jsc-hosted scripts in twins mode, in the reachable heap (harness section 7.6). Section 13.3 adds the conventions: the `main(role, scratch, artifact)` function (`process.argv[2]` to `[4]` under Bun), no role-dependent value left reachable, statistics and imported-state assertions skipped under `Off` while native assertions run in every role, and `jitcache-heap: off` with a reason.
- ucb batch 5.1 section 12 calls the profiler options free while SPEC-image.md section 14 fixes them: fixed. The sentence now says the lane adds no row because `useProfiler`, `useTypeProfiler` and `useControlFlowProfiler` are fixed by SPEC-image.md section 14 and `useSamplingProfiler` by THREAD Storage; a profiler a host turns on at run time, as Bun's coverage does through `VM::enableControlFlowProfiler`, is covered by the key's mode and F16.
- ucb batch 5.2 to 5.5 same profiler-option point, including the ambiguity about `useSamplingProfiler`: fixed, as 5.1, naming every option.
- ucb batch 5.6 a ConsumerProducer's recapture of a decoded UCB seeded from a `Generated` body writes it `EmbedderDecoded`: fixed in the lane. A decoded UCB seeded or attached from a `Generated` body in a VM whose production is active keeps that body's butterfly map in its record (`generatedButterflyMap`, new `copyGeneratedButterflyMap`, a third parameter to `attachPendingImport`). Section 9.2 step 5 then writes provenance `Generated` with that map, because the match proved the core encoding equal to the body's `coreDigest` and a UCB's core never changes. The provenance row of section 7.2, 6.3.3 step 7, 6.3.4 step 7, U4, U8 and `context-miss.js` changed to match. The predicate the third finding suggested for the integrator was not adopted, since the lane can keep the provenance itself. A recaptured decoded function whose holder digest differs from a generated holder's still meets C11 at a generating consumer's import.
- ucb batch 5.7 and 5.8 same provenance point: fixed, as 5.6.
- ucb batch 5.9 `drop-and-reimport.js` cannot see counts drop while Image twin CBs pin the UCBs: fixed. Checked in `CodeBlock::stronglyVisitStrongReferences`, which marks `m_unlinkedCode`, `m_globalObject` and `m_ownerExecutable`, and in SPEC-image.md section 17.2 step 2, which keeps each twin CB until VM destruction. The test's runs now set `--useConcurrentJIT=true`, so the Image check skips without creating a twin and the skip fails nothing.
- ucb batch 5.10 same pinning point, plus the jettisoning stress variant: fixed. The `--useUnlinkedCodeBlockJettisoning=true` stress runs also take `--useConcurrentJIT=true` in twins builds. The suggested sentence for SPEC-image.md section 17.2 is a cross-set item.
- ucb batch 6.1 LLInt counter tests cannot pass for a deferred threshold or negative progress: fixed. Section 8.2 now requires a deferred record to hold the state `deferIndefinitely` leaves (`m_totalCount` 0, `m_counter` `INT32_MIN`), which `armLLIntCounter` reproduces exactly, so I12 and T4 hold for it; U6 tests that record and only non-negative progress for finite T. Section 8.5 notes that no native capture holds a deferred LLInt counter (`shouldJIT` failures and `BaselineJITPlan::finalize` after R-INT-11's fault), checked in `llint/LLIntSlowPaths.cpp`. Rejecting `INT32_MAX` as invalid material was not taken: the state is native and arms exactly.
- ucb batch 6.2 same U6 point: fixed, as 6.1.
- ucb batch 6.3 the parse-time stack-overflow interpretation is not the narrowest choice: fixed. Before its step 1 the import misses `Stack` when `!vm.isSafeToRecurse()`, the limit `Parser::canRecurse` tests, so the native parse throws its `RangeError` as with JITCache off; section 6.3.7 now keeps only a parse whose own recursion would pass the limit. New `MissReason::Stack` and an outcome row.
- ucb batch 6.4 same stack point, suggesting a depth margin as a bench parameter: fixed by 6.3; the margin was rejected, since it would miss imports at depths where the native parse succeeds and still could not predict the parse's own depth.
- ucb batch 6.5 twin generation writes the provider's directives, so T2 cannot catch an import that failed to restore them: fixed. `verifyImport` snapshots the provider's directives before a global or direct-eval twin, compares them with what the twin's parse writes (T2), then restores the snapshot; section 13.2 also names the TDZ environments a direct eval's twin interns, which go with its handles. Checked in `Parser::parse` and `generateUnlinkedCodeBlockImpl`.
- ucb batch 6.6 two test rows expect outcomes the mechanism cannot produce: the `live-attach.js` half was already fixed by batch 3.8. Fixed for `context-miss.js`: its direct-eval case is gone, since `JSScope::collectClosureVariablesUnderTDZ` reads only lexical, catch and module scopes, whose names the caller chain's text fixes; U3 now checks that a direct eval's context digest changes with its TDZ and private-name sets.
- ucb batch 6.7 several JS tests cannot be written as specified: fixed. `with-scope.js` is marked Bun and takes its extension from `vm.compileFunction`, which leaves it set; programs evaluated after it are tainted (`Interpreter::executeProgram`, so the finding's claim that a program never carries the bit was wrong); `role-keys.js`'s extension case moved to a Bun variant; the two-offset case moved to U3; `invalid-material.js`, `holder-miss.js` and `strict-reuse.js` name `jitcacheRewriteSection`, which the integrator provides, and run in twins builds only.
- ucb batch 6.8 JS tests said to run in release builds read twins-only statistics: fixed. Section 13.3 has scripts assert on twins-only helpers only where they exist and declare `// jitcache-requires: twins` when their sequence needs one, and section 13's run matrix says so.
- ucb batch 6.9 U8 needs several roles, both strict values and a sticky fault in one call: fixed. `runUCBSelfTest` takes a `UCBSelfTestScope`; `Configured` runs the parts of U8 the VM's configuration allows, `InvalidMaterial` only the invalid-material case; `$vm.jitCacheUCBSelfTest(options)` (M4) and the new `self-test.js` drive one run per configuration and the invalid-material case last. The alternative of self-tests creating their own VMs was not taken.
- ucb batch 6.10 I5 excludes the Image twin check's writes through `restoreBits`: fixed. I5 names that exception, under SPEC-image.md I20.
- ucb batch 7.1 C5 admits negative link-time constants: fixed. C5 requires 0 <= value < `numberOfLinkTimeConstants`, since `JSGlobalObject::linkTimeConstant` indexes `m_linkTimeConstants` with `static_cast<unsigned>(value)`.
- ucb batch 7.2 an unrequested decoded slot loses its UCB feedback without saying so: fixed by stating it. Section 6.2.2 names what never arrives, what the attach's pending import still brings, and THREAD's basis: no work ahead of demand, seeds only before publication, and the same trade THREAD makes for a root UFE's singleton bit. Seeding that slot when the index lists its body was not adopted, since it reads a body nobody requested.
- ucb batch 7.3 a stored key that differs from the request key is a quiet stamped miss: fixed. Files are named by their whole key and B3 matches the envelope key to the name, so a stored identity key other than `body.key()` is invalid material at `ucb.identity` in both modes (6.3.1 and 6.3.2 step 4, section 11.1). `MissReason::KeyCollision` is gone, and U8 no longer uses it.

## Native-fidelity review after the minors (2026-10-07)

- The root singleton paragraph of section 8.1 called the root's own DFG compile the bit's only consumer: fixed. `ObjectAllocationProfileBase::initializeProfile` (`bytecode/ObjectAllocationProfileInlines.h`) also reads it, through the `FunctionExecutable`'s singleton that `UnlinkedFunctionExecutable::link` derives from the bit: it allocates `this` poly-proto only when both the poly-proto set and the singleton have been invalidated. The paragraph now names both readers and both costs, the jettison and mono-proto allocation until a second function of the executable exists, which are speed only and which a JITCache-off run pays at the same points. The bit stays local, as THREAD Restoration says; THREAD's "the cost is at most one jettison" omits the allocation cost, which goes to the user as a THREAD question.
- The guarantee to the CB, ICs and Image lanes listed fewer index spaces than SPEC-image.md R-UCB-1, extended on 2026-10-07: fixed. The paragraph now lists everything R-UCB-1 names (out-of-line jump targets, exception handlers, each function entry's builtin, arrow and strict bits, `numParameters`, `numCalleeLocals`, `numVars`, the scope register, `codeType` and `isConstructor`) and cites the codec's statement that the core encodes all of it. The guarantee itself held: `CachedCodeBlock` encodes the scope register, the frame counts, `isConstructor`, `codeType` and the out-of-line jump targets.
- Cross-set item from the integrator's batch 5, the `request` bench event: done. B2 now records its measurements with `BenchReport::record` in the `request` event of SPEC-integrator.harness.md section 9.2 when `VM::jitCacheState()->benchReport()` is non-null, with the fields that event lists, so IB3 can join it with `install` by key.
- Cross-set item from the integrator's batch 6, R-ALL-8: done. Section 3 now says that unified sources bundle the lane's files with the other parts' `jitcache/*.cpp` files, so every file-local helper and constant begins with `ucb` or sits in a per-file named namespace, as SPEC-ics.md section 7 says for `ics`.
- Cross-set item, M4 with no twin report open: done. `verifyRegistry` now takes `TwinReportSink*` and returns the violation count whether or not a report is open, reporting each violation only through a non-null sink, as SPEC-integrator.md R-ALL-2 allows; M4 passes `VMState::twinReportSink()`, so `registryViolations` and the stress line's check work without a report path. A local in-memory sink was not adopted, since `TwinReport` is file-backed and the count alone answers the check.
- Cross-set item from the image lane's batch 8: done. SPEC-image.md I20 and section 17.2 step 2 now keep a twin CB alive only for its check, so the pinning that batches 5.9 and 5.10 worked around is gone: `drop-and-reimport.js` and the jettisoning stress runs no longer set `--useConcurrentJIT=true`, and they keep the image twin check.
- Cross-set item from integrator batch 7.4: done. SPEC-integrator.harness.md section 9.1 now reads the thread CPU clock, a system call on Linux, only where a counted span begins or ends, and breaks a counted span into steps with `CLOCK_MONOTONIC`. B2's `request` event follows that rule, so its per-part times no longer inflate the bound they explain.

## Compaction (2026-10-07)

The set was shortened without changing what it requires. Section numbers stay; two sections were dissolved or removed and two renamed. Every identifier keeps its name: F1 to F26, I1 to I26, R-INT-1 to R-INT-11, C1 to C11, S1, S2, SV1 to SV3, SC1, SC2, U1 to U9, T1 to T7, B1 to B10, M1 to M6, E1 to E15 and tasks 0 to 12.

| old (HEAD) | new |
|---|---|
| SPEC-ucb.md | SPEC-ucb.md |
| SPEC-ucb.md, preamble | preamble |
| §1 Scope and summary | §1; the list of what lies outside the lane is replaced by a citation of THREAD Execution, and the one-paragraph summary became a shorter overview that points to sections |
| §2 Native facts (F1 to F26) | §2, unchanged |
| §3 Owned paths | §3; its sentence on `DebuggerCallFrame::evaluateWithScopeExtension` lives in §6.2.4, which already stated it |
| §4 Keys, identities and contexts; §4.1 to §4.6 | §4; §4.1 to §4.6. §4.2 keeps the decoded-root clause in short and cites §6.3.3 step 1, which holds it in full with its THREAD Identity and F4 citations |
| §5 The parent-key registry, pending imports and statistics; §5.1 to §5.6 | §5; §5.1 to §5.6 |
| §6 Request points; §6.1; §6.2 and §6.2.1 to §6.2.8 | §6; §6.1; §6.2 and §6.2.1 to §6.2.8 |
| §6.3 The engine; §6.3.1 to §6.3.6 | §6.3; §6.3.1 to §6.3.6 |
| §6.3.7 Interpretations of THREAD Identity | dissolved. Reuse of a live UCB and its comparison through the record: §6.3.4 intro and step 4. The snapshot bits: §6.1. Key and reference closure: §11.2 intro. What a context holds and which UCBs a body serves: §4.3 intro, §6.3.2 and I18. The with-scope rule: §6.3.3 step 1 and §6.3.4 step 4. A Function-constructor UFE served by the CodeCache: §6.2.5. Parse-time stack depth: §6.3.1, the paragraph before step 1 |
| §6.4 Parse fields; §6.5 Publication | §6.4; §6.5 |
| §6.6 Contexts | §6.6 Threads, locks and GC (renamed) |
| §6.7 Engine interface | §6.7 |
| §7 The lane's sections of a body file; §7.1 to §7.5 | §7; §7.1 to §7.5 |
| §8 UCB feedback; §8.1 to §8.7 | §8; §8.1 to §8.7 |
| §9 Capture; §9.1; §9.2 | §9; §9.1; §9.2, whose step 5 is split into Provenance, Digests and Maps |
| §10 Interfaces; §10.1; §10.2 | §10; §10.1; §10.2 |
| §11 Failures and strict checks; §11.1; §11.2 | §11; §11.1; §11.2 |
| §12 Options to §17 Tasks | §12 to §17 |
| §18 Notes | removed. Each argument is in this file: the hash-only walk and the lazy validation in revision 5, build-time holder digests in revision 8, direct-eval contexts in revision 9, per-body text digests, the Bun ownership fix, keying a decoded root by its request's bits and operand checks in the drain after the thread-prep run, and the start column in revision 3 |
| SPEC-ucb.codec.md | SPEC-ucb.codec.md |
| SPEC-ucb.codec.md, preamble | preamble |
| codec §1 Why the bytecode cache's codec | codec §1 The core (renamed) |
| codec §2 Entry points to §7 Tests (U7) | codec §2 to §7 |
| SPEC-ucb-history.md | SPEC-ucb-history.md, with this section appended |

The records above this section cite SPEC-ucb.md by the section numbers it had when they were written. Where they cite §6.3.7, §18 or "Notes", including the THREAD-gap lists of revisions 1, 2 and 5 to 9, the rows for those sections give the new place.

The first review of the compaction found wording lost at §4.3, §4.4, §6.2.3, §6.3.1 step 4, §6.7, §7.2, §7.4, §10.2 and the `with-scope.js` row of §13.3. Each was restored in its place, together with a THREAD Session citation dropped from §4.4's supplied-digest paragraph. No place in the map changed.

## Report drain (2026-10-07)

- `ucb-butterfly-map-kept-map-not-in-layout-or-writer.md`: fixed. §9.2 step 5 writes a decoded UCB's kept map as its butterfly map, but §7.2 defined the map only by the constants' structure, said a decoded UCB always writes an empty map, and §7.5's `writeIdentitySection` had no way to receive the kept map. §7.5 gains `std::optional<std::span<const uint8_t>> keptButterflyMap`, which replaces the map computed from the constants; §7.2's row and sentence count a kept map; §9.2 step 5 passes it, copied into a ⌈N/8⌉-byte buffer charged to the budget and released after the write, and its Maps bullet names the exception.
- `ucb-source-digests-counter-scope.md`: fixed. §6.7 counted every source digest computed from text, a direct eval's text digest included, while the §5.6 comment and B1 read `sourceDigests` as root digests (form 3 of §4.4). The root reading stands, since B1 compares it with `suppliedSourceDigests`, which counts only roots: §6.7 and the §5.6 comment now count root source digests computed from text and say a direct eval's is not counted.
- `ucb-statistics-misses-array-size.md`: fixed. `MissReason` has ten enumerators, and §5.6 declared `misses` with eleven slots, left over from the removal of `KeyCollision` in batch 7.3 of the kept minors; the array now has ten. The report's alternative, a count constant beside the enum, would add a name for a mismatch nothing else has, since `records` and `invalidMaterial` state their sizes the same way.

## Walkthrough reports

- `spec-ucb-pointers-after-compaction.md`, item 1: fixed. §8.3 placed the seeding of a natively decoded UCB at step 7 of §6.3.3, which records the UCB; seeding is step 6, and §8.3 now cites it.
- `spec-ucb-pointers-after-compaction.md`, item 2: fixed. The §11.1 row for C10 on a live decoded UCB cited §6.3.4 step 5 for the write of `matchedBodyVersion`, but that step only compares the field with the opened file's identifier; the paragraph after §6.3.2's steps, on a miss at its step 9, writes it. The row now cites §6.3.2 for the write and §6.3.4 step 5 for the next request's repeat of C10.
- `spec-ucb-pointers-after-compaction.md`, item 3: fixed. §4.4 said every twins build checks the supplied digests it takes (T7), but §13.2 calls the verify functions only while `S.twinReportSink()` is non-null, and the integrator opens a twin report only when `Config::twinReportPath` is set (SPEC-integrator.harness.md §3.1). §4.4 now says twins builds with a twin report open.
- `spec-ucb-pointers-after-compaction.md`, item 4: fixed. R-INT-10 put the runner under `ENABLE(JITCACHE_TWINS)`, yet §13 also runs the tests through it in a release build without twins, which the runner's plain mode serves (SPEC-integrator.harness.md §7.1). R-INT-10 now asks for the runner in every build and for `twinReportSink()` in twins builds only, non-null while a twin report is open, the integrator's condition, in place of "while the twin runner drives the VM".
- `spec-ucb-pointers-after-compaction.md`, item 5: fixed. §4.6 said the self-test checks NIST's short and long message vectors, which U1 does not list; §4.6 now says what U1 runs, the FIPS 180-4 examples on every path and a comparison of the paths over random lengths. U1 was not extended: U9 already compares the runtime digest with the build's `hashlib` digest of every builtin's source, a known-answer check over many lengths, and no case shows a gap the NIST sets would close.
- `spec-ucb-pointers-after-compaction.md`, item 6: fixed. Codec §3 listed the decoder failures a decode records as E4, E5 and E15, while E14 records `Malformed` for a bad start record too, as C6, §11.1 and codec E15 already said. Codec §3 now lists E14.
- `spec-ucb-r-int-3-omits-integrity-checks.md`: fixed. §11.2, codec §4 and §6.3.1 step 4 rest normal mode on the container's integrity checks and cite R-INT-3 or the container for them, but R-INT-3 asked only for a lookup. SPEC-integrator.container.md §4.5 runs B1, B3, B4, B8 and the integrity parts of B2 and B6 in both modes, and `Full` adds B5, B7 and the structure parts of B2 and B6; SPEC-integrator.md §7.2 already returns `Found` only after them. R-INT-3 now states the guarantee the lane relies on: in both modes the requested key, the artifact's header digest and every checksum, and under strict the structure checks too. The integrator needs no change.
- `spec-ucb-unlinkedcodeblock-destructor-ownership.md`: fixed. SPEC-integrator.md M10 adds a twins-only `JITCache::retireBodyEventCounts(*this)` to `~UnlinkedCodeBlock`, right after this lane's hook, while §3 said no other part edits the members it lists. The row now names that edit, and the rule admits an edit its row names. §5.5's hook still runs first, and M10 states that retiring reads no registry entry. The data member and accessor M10 adds to `UnlinkedCodeBlock.h` were never in conflict, since that row lists only the two arithmetic-profile counts.

## Options table

- §12's rows, its free options and its note on profilers a host turns on at run time moved to docs/JitCache/options.md, the one table the five sets share; §12 points there and keeps the build-time capture of default values, and F16, §10.1, R-INT-9, M5 and codec §6 cite options.md. No option changed class, value or type.

## Runner directives

- R-ALL-4's `jitcache-expect-fault` and `jitcache-expect-no-install`, which harness §7.5 now enforces, are declared in §13.3: `self-test.js` expects `ucb.supplied-digest` at run 4 and waives installs at runs 1 to 4; `invalid-material.js` expects `ucb.identity`, `ucb.feedback`, `ucb.decode` and `ucb.closure` at its consumer, run 1, which installs nothing; `strict-reuse.js` expects `ucb.strict-core` where strict meets the generated UCB; `context-miss.js` (run 1 of its reverse sequence), `start-line.js` (the consumer under `lineOffset` 10, run 2), `role-keys.js`, `compile-function.js`, `live-attach.js` and `holder-miss.js` waive installs in the runs they leave with nothing to install. No test checks anything new.
