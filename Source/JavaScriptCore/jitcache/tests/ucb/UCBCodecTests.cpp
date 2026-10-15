#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BuiltinExecutables.h"
#include "ByteFields.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DirectEvalExecutable.h"
#include "IndirectEvalExecutable.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Options.h"
#include "RegExp.h"
#include "RegExpCache.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
#include "UCBFeedback.h"
#include "UCBKeys.h"
#include "UCBTestSupport.h"
#include "UCBTwinGeneration.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/InlineMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The tests of the core codec runtime/CachedTypes.cpp gives the UCB lane: the InlineMap transport of declaration maps; the
// round trip of every root record and function core, decoded without and with validation; chunked encodes and encoding
// budgets; what the native bytecode cache's decodes encode to; RegExp constants and the ordered-hash-table sentinel; TDZ
// chains against the holder's; descriptors; the payload header's failures; and, through runCoreCodecSelfTest, the cases that
// alter a core's records.

namespace JSC::JITCache::Tests {

namespace UCBCodecTestsInternal {

static std::optional<Vector<uint8_t>> encodeDescriptorBytes(VM& vm, const UnlinkedFunctionExecutable& executable)
{
    CoreEncodeFailure failure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, executable, nullptr, failure);
    if (!descriptor)
        return std::nullopt;
    return Vector<uint8_t> { descriptor->span() };
}

// Decodes a copy of `bytes`, which starts 8-byte aligned in a buffer the payload owns, as the import's body does.
static UnlinkedCodeBlock* decodeCoreBytes(VM& vm, std::span<const uint8_t> bytes, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, bool validate, CoreDecodeFailure& failure)
{
    Vector<uint8_t> copy { bytes };
    std::span<uint8_t> data = copy.mutableSpan();
    Ref<CachedBytecode> payload = CachedBytecode::create(data, [copy = WTF::move(copy)](const void*) { }, { });
    failure = CoreDecodeFailure::None;
    return decodeUnlinkedCodeBlockCore(vm, WTF::move(payload), provider, kind, holder, validate, failure);
}

// The map a VariableEnvironment keeps its declarations in, which the codec transports. VariableEnvironment names it Map
// privately, for CachedVariableEnvironment alone, so the test spells it from its public parts, and the assertion holds the
// spelling to the map VariableEnvironment iterates. Its key traits are RefPtr's while its key is a PackedRefPtr, which
// cannot store the deleted value those traits make, so this map takes no removal.
using DeclarationMap = InlineMap<PackedRefPtr<UniquedStringImpl>, VariableEnvironmentEntry, VariableEnvironment::inlineMapCapacity, IdentifierRepHash,
    HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits>;
static_assert(std::is_same_v<DeclarationMap::iterator, decltype(std::declval<VariableEnvironment&>().begin())>);

// A map under the same hash whose key traits match its key, so that removals leave the deleted buckets a layout carries.
using RemovableMap = InlineMap<RefPtr<UniquedStringImpl>, VariableEnvironmentEntry, VariableEnvironment::inlineMapCapacity, IdentifierRepHash,
    HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits>;

static VariableEnvironmentEntry declarationEntryFor(unsigned keyIndex)
{
    VariableEnvironmentEntry entry;
    if (keyIndex % 2)
        entry.setIsVar();
    else
        entry.setIsLet();
    return entry;
}

template<typename Map>
static Vector<const UniquedStringImpl*> iterationOrder(const Map& map)
{
    Vector<const UniquedStringImpl*> order;
    for (const auto& entry : map)
        order.append(entry.key.get());
    return order;
}

// Whether two maps iterate alike and have the same storage, capacity and deleted count.
template<typename Map>
static bool sameLayout(Map& a, Map& b)
{
    using Access = WTF::InlineMapAccessForTesting;
    return iterationOrder(a) == iterationOrder(b) && Access::isInline(a) == Access::isInline(b) && Access::capacity(a) == Access::capacity(b)
        && Access::deletedCount(a) == Access::deletedCount(b);
}

// What a serializer records of a hashed map: its capacity, each occupied bucket as (index << 1) | isDeleted, and the
// entries of the live buckets in index order.
template<typename Map>
struct InlineMapLayout {
    unsigned capacity { 0 };
    Vector<uint32_t> slots;
    Vector<typename Map::Entry> entries;
};

template<typename Map>
static std::optional<InlineMapLayout<Map>> layoutOf(const Map& map)
{
    if (!map.usesHashedStorage())
        return std::nullopt;
    InlineMapLayout<Map> layout;
    layout.capacity = map.hashedCapacity();
    map.forEachOccupiedBucket([&](unsigned index, bool isDeleted, const typename Map::Entry* entry) {
        layout.slots.append((index << 1) | (isDeleted ? 1 : 0));
        if (!isDeleted)
            layout.entries.append(*entry);
    });
    return layout;
}

// A layout of `count` keys, taken in order from keys[firstKey] on, each in the bucket where its probe sequence starts in a
// table of `capacity` buckets, with no deleted bucket. Every lookup finds its key at once, so the layout meets each
// condition of restoreHashedLayout that does not weigh the capacity against the occupancy.
static std::optional<InlineMapLayout<RemovableMap>> directLayout(const Vector<Identifier>& keys, unsigned firstKey, unsigned capacity, unsigned count)
{
    Vector<std::pair<unsigned, unsigned>> placed; // bucket, key index
    Vector<bool> taken(FillWith { }, capacity, false);
    for (unsigned i = firstKey; i < keys.size() && placed.size() < count; ++i) {
        unsigned bucket = IdentifierRepHash::hash(keys[i].impl()) & (capacity - 1);
        if (taken[bucket])
            continue;
        taken[bucket] = true;
        placed.append({ bucket, i });
    }
    if (placed.size() < count)
        return std::nullopt;
    std::sort(placed.begin(), placed.end());
    InlineMapLayout<RemovableMap> layout;
    layout.capacity = capacity;
    for (auto [bucket, keyIndex] : placed) {
        layout.slots.append(bucket << 1);
        layout.entries.append(RemovableMap::Entry { keys[keyIndex].impl(), declarationEntryFor(keyIndex) });
    }
    return layout;
}

template<typename Map>
static bool refusedAndEmpty(Map& map, unsigned capacity, std::span<const uint32_t> slots, Vector<typename Map::Entry>&& entries)
{
    bool restored = map.restoreHashedLayout(capacity, slots, WTF::move(entries));
    return !restored && map.isEmpty() && !map.usesHashedStorage();
}

// Restores the layout of `map`, a hashed map holding the keys that `present` marks, into a fresh map, which must equal the
// original in iteration order, capacity and deleted count, find exactly the original's keys, and evolve as the original
// does under the same later additions and, for a map that takes them, removals.
template<typename Map>
static bool restoresExactly(TestPart& part, Map& map, Vector<bool>& present, const Vector<Identifier>& keys, WeakRandom& random, ASCIILiteral mapName)
{
    static constexpr bool takesRemovals = std::is_same_v<Map, RemovableMap>;
    unsigned size = map.size();
    auto layout = layoutOf(map);
    if (!part.check(!!layout, makeString("a "_s, mapName, " of "_s, size, " entries does not use hashed storage"_s)))
        return false;
    Map restored;
    bool accepted = restored.restoreHashedLayout(layout->capacity, layout->slots.span(), Vector<typename Map::Entry> { layout->entries });
    if (!part.check(accepted && sameLayout(restored, map), makeString("a hashed "_s, mapName, " of "_s, size, " entries does not restore to the same iteration order, capacity and deleted count"_s)))
        return false;
    bool lookups = true;
    for (unsigned i = 0; i < keys.size(); ++i)
        lookups &= restored.contains(keys[i].impl()) == present[i];
    if (!part.check(lookups, makeString("a restored "_s, mapName, " of "_s, size, " entries does not find exactly its keys"_s)))
        return false;
    for (unsigned step = 0; step < 60; ++step) {
        unsigned i = random.getUint32(static_cast<unsigned>(keys.size()));
        if (present[i]) {
            if constexpr (takesRemovals) {
                map.remove(keys[i].impl());
                restored.remove(keys[i].impl());
            } else
                continue;
        } else {
            map.add(keys[i].impl(), declarationEntryFor(i));
            restored.add(keys[i].impl(), declarationEntryFor(i));
        }
        present[i] = !present[i];
    }
    return part.check(sameLayout(restored, map), makeString("a restored "_s, mapName, " of "_s, size, " entries evolves unlike the original under the same additions and removals"_s));
}

// String-keyed maps of 10 to 500 entries restore exactly and then evolve alike: a map whose key traits match its key, with
// random removals, and VariableEnvironment's own map, which takes none. Each condition of restoreHashedLayout refuses
// once and leaves the map empty and inline.
static bool testInlineMapTransport(VM& vm, TestPart& part)
{
    Vector<Identifier> keys;
    for (unsigned i = 0; i < 1100; ++i)
        keys.append(Identifier::fromString(vm, makeString("jitcacheU7Key"_s, i)));
    static constexpr std::array<unsigned, 6> sizes { 10, 13, 40, 97, 256, 500 };

    unsigned hashedLayouts = 0;
    std::optional<InlineMapLayout<RemovableMap>> refusalBase;
    for (unsigned size : sizes) {
        WeakRandom random(size);
        RemovableMap map;
        Vector<bool> present(FillWith { }, keys.size(), false);
        for (unsigned i = 0; i < size; ++i) {
            map.add(keys[i].impl(), declarationEntryFor(i));
            present[i] = true;
        }
        for (unsigned i = 0; i < size; ++i) {
            if (!random.getUint32(3)) {
                map.remove(keys[i].impl());
                present[i] = false;
            }
        }
        auto layout = layoutOf(map);
        if (!layout)
            continue; // removals shrank it back to inline storage, which re-adding reproduces
        ++hashedLayouts;
        if (!refusalBase && layout->slots.size() > layout->entries.size() && layout->entries.size() > 4)
            refusalBase = WTF::move(layout);
        if (!restoresExactly(part, map, present, keys, random, "map with removals"_s))
            return false;
    }
    if (!part.check(hashedLayouts >= 4 && !!refusalBase, "too few maps with removals kept hashed storage with deleted buckets"_s))
        return false;

    // VariableEnvironment's map, every size past its inline capacity, so every one is hashed.
    for (unsigned size : sizes) {
        WeakRandom random(size + 1);
        DeclarationMap map;
        Vector<bool> present(FillWith { }, keys.size(), false);
        for (unsigned i = 0; i < size; ++i) {
            map.add(keys[i].impl(), declarationEntryFor(i));
            present[i] = true;
        }
        if (!restoresExactly(part, map, present, keys, random, "declaration map"_s))
            return false;
    }

    // Each condition broken once, from a valid layout with deleted buckets.
    const InlineMapLayout<RemovableMap>& base = *refusalBase;
    auto refuses = [&](ASCIILiteral condition, unsigned capacity, const Vector<uint32_t>& slots, Vector<RemovableMap::Entry> entries) {
        RemovableMap map;
        return part.check(refusedAndEmpty(map, capacity, slots.span(), WTF::move(entries)), makeString("restoreHashedLayout accepts "_s, condition, " or leaves the map non-empty"_s));
    };
    {
        RemovableMap map;
        if (!part.check(map.restoreHashedLayout(base.capacity, base.slots.span(), Vector<RemovableMap::Entry> { base.entries }), "restoreHashedLayout refuses the layout the refusal cases start from"_s))
            return false;
    }
    Vector<uint32_t> swapped = base.slots;
    std::swap(swapped[0], swapped[1]);
    Vector<uint32_t> pastCapacity = base.slots;
    pastCapacity.last() = (base.capacity << 1) | (pastCapacity.last() & 1);
    Vector<RemovableMap::Entry> fewer = base.entries;
    fewer.removeLast();
    Vector<RemovableMap::Entry> duplicated = base.entries;
    duplicated[1] = duplicated[0];
    Vector<RemovableMap::Entry> nullKey = base.entries;
    nullKey[0].key = nullptr;
    // A live entry moved to an empty bucket. In the valid layout every empty bucket lies past the entry's own on its probe
    // sequence, or the probe would have stopped there, so the entry's own bucket, now empty, stops the probe first.
    Vector<uint32_t> movedSlots;
    Vector<RemovableMap::Entry> movedEntries;
    {
        Vector<std::pair<uint32_t, std::optional<size_t>>> buckets; // slot, and the entry a live slot holds
        size_t nextEntry = 0;
        for (uint32_t slot : base.slots)
            buckets.append({ slot, (slot & 1) ? std::nullopt : std::optional<size_t> { nextEntry++ } });
        Vector<bool> occupied(FillWith { }, base.capacity, false);
        for (uint32_t slot : base.slots)
            occupied[slot >> 1] = true;
        unsigned target = 0;
        while (occupied[target])
            ++target;
        for (auto& bucket : buckets) {
            if (bucket.second) {
                bucket.first = target << 1;
                break;
            }
        }
        std::sort(buckets.begin(), buckets.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (auto& bucket : buckets) {
            movedSlots.append(bucket.first);
            if (bucket.second)
                movedEntries.append(base.entries[*bucket.second]);
        }
    }
    // Load and minimum load, from layouts whose keys each sit where their probe starts, so that nothing else is wrong with
    // them: one with the same occupancy restores, while thirteen live buckets in sixteen pass the most a map built by add()
    // holds, and 128 buckets for eleven entries pass minLoadInverse (6) times the entries, which a removal would shrink.
    auto loadControl = directLayout(keys, 1000, 64, 11);
    auto crowded = directLayout(keys, 1000, 16, 13);
    auto sparse = directLayout(keys, 1000, 128, 11);
    if (!part.check(loadControl && crowded && sparse, "the load test layouts could not be built from the test keys"_s))
        return false;
    {
        RemovableMap map;
        if (!part.check(map.restoreHashedLayout(loadControl->capacity, loadControl->slots.span(), Vector<RemovableMap::Entry> { loadControl->entries }), "restoreHashedLayout refuses a layout whose keys sit where their probes start"_s))
            return false;
    }

    // The deleted value as a key. A RefPtr holding it must be neither copied nor destroyed, which would ref or deref it, so
    // the entry is made in place, and its key is leaked once the refusal has left the vector untouched.
    bool refusesDeletedKey = false;
    {
        Vector<RemovableMap::Entry> deletedKey = base.entries;
        deletedKey[0].key = RefPtr<UniquedStringImpl> { WTF::HashTableDeletedValue };
        RemovableMap map;
        bool restored = map.restoreHashedLayout(base.capacity, base.slots.span(), WTF::move(deletedKey));
        refusesDeletedKey = !restored && map.isEmpty() && !map.usesHashedStorage() && deletedKey.size() == base.entries.size();
        std::ignore = deletedKey[0].key.leakRef();
    }
    if (!part.check(refusesDeletedKey, "restoreHashedLayout accepts the deleted value as a key or leaves the map non-empty"_s))
        return false;

    return refuses("a capacity that is not a power of two"_s, base.capacity + 1, base.slots, base.entries)
        && refuses("a capacity no larger than the inline capacity"_s, 8, base.slots, base.entries)
        && refuses("slot indices out of order"_s, base.capacity, swapped, base.entries)
        && refuses("a slot index at the capacity"_s, base.capacity, pastCapacity, base.entries)
        && refuses("fewer entries than live slots"_s, base.capacity, base.slots, fewer)
        && refuses("a duplicated key"_s, base.capacity, base.slots, duplicated)
        && refuses("an empty key"_s, base.capacity, base.slots, nullKey)
        && refuses("an entry its probe sequence cannot reach"_s, base.capacity, movedSlots, movedEntries)
        && refuses("more live buckets than the load factor allows"_s, crowded->capacity, crowded->slots, crowded->entries)
        && refuses("a capacity above minLoadInverse times the entries"_s, sparse->capacity, sparse->slots, sparse->entries);
}

// Programs in the manner of JSTests/stress, together covering the forms a core encodes: closures and loops, integer,
// character and string switches, exception handlers, classes with fields, private names, static blocks and super, generators
// and async functions, destructuring and spread, regular expressions, big integers, doubles, templates, butterflies, a literal
// whose elements the parser folds to NaNs of both signs, more than nine global declarations, with and eval, accessors and
// computed keys, and nested block scopes.
static constexpr std::array<ASCIILiteral, 7> codecCorpus { {
    "function jitcacheU7Switch(x, s) {\n"
    "  var total = 0;\n"
    "  outer: for (var i = 0; i < x; i++) {\n"
    "    switch (i % 5) { case 0: total += 1; break; case 1: total -= 2; break; case 2: case 3: total *= 2; break; default: continue outer; }\n"
    "    switch (s[i % s.length]) { case 'a': total++; break; case 'b': total--; break; }\n"
    "    switch (s) { case 'alpha': total += 3; break; case 'beta': total += 4; break; case 'gamma': total += 5; break; }\n"
    "  }\n"
    "  try { if (total > 100) throw new Error('jitcache-u7-big'); } catch (e) { total = -1; } finally { total += arguments.length; }\n"
    "  return function inner(y) { return total + y; };\n"
    "}\n"
    "var jitcacheU7SwitchResult = jitcacheU7Switch(10, 'abba');\n"_s,
    "class JitcacheU7Base { #secret = 1; static count = 0; static { JitcacheU7Base.count++; } get secret() { return this.#secret; } #hidden() { return 2; } reveal() { return this.#hidden() + this.#secret; } }\n"
    "class JitcacheU7Derived extends JitcacheU7Base { extra = [\"alpha\", \"beta\"]; constructor() { super(); this.y = 3; } static make() { return new JitcacheU7Derived(); } }\n"
    "let jitcacheU7Instance = JitcacheU7Derived.make();\n"_s,
    "function* jitcacheU7Gen(n) { for (let i = 0; i < n; i++) yield i; }\n"
    "async function jitcacheU7Async(a, { b, c = 2 } = {}, ...rest) { const [d, ...e] = await Promise.resolve([a, b, c]); return d?.toString() ?? e.length + rest.length; }\n"
    "async function* jitcacheU7AsyncGen() { yield* jitcacheU7Gen(3); }\n"
    "const jitcacheU7Spread = [...jitcacheU7Gen(4), ...\"xyz\"];\n"_s,
    "var jitcacheU7Literals = { re: /jit(cache)+\\d*/gi, big: 12345678901234567890123n, d: 1.5, n: NaN, z: -0, inf: Infinity, words: [\"one\", \"two\", \"three\"], mixed: [\"one\", 2], hole: [1, , 3],\n"
    "  nans: [1e400 * 0, -(1e400 * 0)] };\n"
    "function jitcacheU7Tag(strings, ...values) { return strings.raw.join('|') + values.join(','); }\n"
    "var jitcacheU7Template = jitcacheU7Tag`a${1}b${2}c` + `plain ${jitcacheU7Literals.d}`;\n"_s,
    "var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11, v12 = 12;\n"
    "let l0 = 0, l1 = 1, l2 = 2, l3 = 3, l4 = 4, l5 = 5, l6 = 6, l7 = 7, l8 = 8, l9 = 9, l10 = 10;\n"
    "function jitcacheU7Sloppy(o, code) { with (o) { var r = x; } return eval(code) + r; }\n"_s,
    "var jitcacheU7Object = { get g() { return 1; }, set s(v) { this._s = v; }, ['computed' + 1]: 2, [Symbol.iterator]: function* () { yield 1; } };\n"
    "function jitcacheU7Walk(o) { var keys = []; for (var k in o) keys.push(k); for (var v of [1, 2]) keys.push(v); delete o.g; return ('s' in o) && (o instanceof Object) && typeof o; }\n"_s,
    // Two sibling functions of one block, whose chains of two links share both, and an arrow in each.
    "'use strict';\n"
    "{ let jitcacheU7Outer = 1; { const jitcacheU7Inner = 2;\n"
    "  function jitcacheU7Nested() { return () => jitcacheU7Outer + jitcacheU7Inner; }\n"
    "  function jitcacheU7Sibling() { return () => jitcacheU7Inner; }\n"
    "  jitcacheU7Nested(); jitcacheU7Sibling(); } }\n"_s,
} };
static constexpr size_t nestedScopesCorpusIndex = 6;
static_assert(nestedScopesCorpusIndex + 1 == codecCorpus.size());

// The codec's other root records: a module, whose var declarations, its import among them, outnumber an InlineMap's inline
// capacity, so its core crosses the hashed layout; an indirect eval with var and function declarations and a sloppy block
// function, which the eval keeps as a hoisting candidate; and a direct eval under a TDZ set, whose function's chain starts
// at the link over that set and goes into the core whole, since an eval core has no holder.
static constexpr ASCIILiteral moduleCorpusText = "import { jitcacheU7Imported } from 'jitcache-u7-module-dependency';\n"
    "var jitcacheU7M0 = 0, jitcacheU7M1 = 1, jitcacheU7M2 = 2, jitcacheU7M3 = 3, jitcacheU7M4 = 4, jitcacheU7M5 = 5;\n"
    "var jitcacheU7M6 = 6, jitcacheU7M7 = 7, jitcacheU7M8 = 8, jitcacheU7M9 = 9, jitcacheU7M10 = 10, jitcacheU7M11 = 11;\n"
    "let jitcacheU7ModuleLet = 'jitcache-u7-module-let';\n"
    "const jitcacheU7ModuleConst = [jitcacheU7M0, jitcacheU7Imported];\n"
    "export function jitcacheU7ModuleExported(a) { return a + jitcacheU7M1 + jitcacheU7ModuleLet.length; }\n"
    "export default class JitcacheU7ModuleDefault { static m() { return jitcacheU7ModuleConst; } }\n"_s;
static constexpr ASCIILiteral indirectEvalCorpusText = "var jitcacheU7EvalA = 1, jitcacheU7EvalB = 'jitcache-u7-eval-string';\n"
    "function jitcacheU7EvalFunction(a) { return a + jitcacheU7EvalA; }\n"
    "if (jitcacheU7EvalA) { function jitcacheU7EvalHoisted() { return jitcacheU7EvalB; } }\n"
    "let jitcacheU7EvalLet = [jitcacheU7EvalA, jitcacheU7EvalB];\n"
    "jitcacheU7EvalFunction(jitcacheU7EvalLet.length);\n"_s;
static constexpr ASCIILiteral directEvalCorpusText = "let jitcacheU7DirectLet = 2;\n"
    "function jitcacheU7DirectFunction() { return jitcacheU7DirectOuter + jitcacheU7DirectLet; }\n"
    "var jitcacheU7DirectVar = jitcacheU7DirectFunction;\n"_s;

// A program whose core spans several pages.
static String bigProgramText()
{
    StringBuilder text;
    for (unsigned i = 0; i < 400; ++i)
        text.append("function jitcacheU7Big"_s, i, "(a) { return a + \"jitcache-u7-big-constant-"_s, i, "\"; }\n"_s);
    return text.toString();
}

// A class whose field initializer's descriptor spans several pages.
static String manyFieldsText()
{
    StringBuilder text;
    text.append("class JitcacheU7Fields { "_s);
    for (unsigned i = 0; i < 1500; ++i)
        text.append("jitcacheU7Field"_s, i, " = 0; "_s);
    text.append("}\n"_s);
    return text.toString();
}

// The chain shape the twin check compares: the number of links, and where the chain reaches `target`.
struct ChainShape {
    unsigned length { 0 };
    std::optional<unsigned> targetPosition;
    friend bool operator==(const ChainShape&, const ChainShape&) = default;
};

static ChainShape chainShape(TDZEnvironmentLink* link, const TDZEnvironmentLink* target)
{
    ChainShape shape;
    for (; link; link = link->parent()) {
        if (target && link == target && !shape.targetPosition)
            shape.targetPosition = shape.length;
        ++shape.length;
    }
    return shape;
}

// Whether a declaration map holds a symbol key. A symbol's hash is per process, so a hashed map holding one would lay out
// differently in another process, and the tests assert that no corpus program declares one.
static bool holdsSymbolKey(const VariableEnvironment& environment)
{
    for (const auto& entry : environment) {
        if (entry.key->isSymbol())
            return true;
    }
    return false;
}

static bool sameDeclarationOrder(const VariableEnvironment& a, const VariableEnvironment& b)
{
    if (a.mapSize() != b.mapSize())
        return false;
    auto other = b.begin();
    for (const auto& entry : a) {
        if (other == b.end() || entry.key.get() != other->key.get() || !(entry.value == other->value))
            return false;
        ++other;
    }
    return true;
}

static bool isAtomString(JSValue value)
{
    auto* string = value ? dynamicDowncast<JSString>(value) : nullptr;
    StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
    return impl && impl->isAtom();
}

static bool allStringConstantsAreAtoms(UnlinkedCodeBlock& codeBlock)
{
    for (auto& constant : codeBlock.constantRegisters()) {
        JSValue value = constant.get();
        if (value && value.isString() && !isAtomString(value))
            return false;
    }
    return true;
}

// What the twin checks compare between a UCB and the UCB its core decodes to: the same encoding, every string constant
// register an atom, the same global parse fields and declaration orders, the same feedback layout, and children with the
// same fields, unparsed, whose chains have the same shape over the holder's.
static bool sameAsDecoded(VM& vm, TestPart& part, UnlinkedCodeBlock& original, UnlinkedCodeBlock& decoded, const UnlinkedFunctionExecutable* holder, std::span<const uint8_t> core, ASCIILiteral what)
{
    auto encoding = encodeCoreBytes(vm, decoded, holder);
    if (!part.check(encoding && equalSpans(encoding->span(), core), makeString("decoding and re-encoding "_s, what, " does not give its core back"_s)))
        return false;
    if (!part.check(allStringConstantsAreAtoms(decoded), makeString("a string constant register of decoded "_s, what, " is not an atom"_s)))
        return false;
    if (!part.check(liveFeedbackCounts(original) == liveFeedbackCounts(decoded), makeString("decoded "_s, what, " has other profile, child or constant counts"_s)))
        return false;
    // UnlinkedGlobalCodeBlock declares no ClassInfo, so a dynamic cast to it accepts a function UCB, whose cell lacks these
    // fields. A function body's parse fields are its UFE's; they travel in the identity section, and both sides share the
    // holder.
    if (!is<UnlinkedFunctionCodeBlock>(original)) {
        auto& originalGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(original);
        auto& decodedGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(decoded);
        bool sameParse = originalGlobal.codeFeatures() == decodedGlobal.codeFeatures() && originalGlobal.lexicallyScopedFeatures() == decodedGlobal.lexicallyScopedFeatures()
            && originalGlobal.hasCapturedVariables() == decodedGlobal.hasCapturedVariables() && originalGlobal.lineCount() == decodedGlobal.lineCount()
            && originalGlobal.endColumn() == decodedGlobal.endColumn() && WTF::equal(originalGlobal.sourceURLDirective(), decodedGlobal.sourceURLDirective())
            && WTF::equal(originalGlobal.sourceMappingURLDirective(), decodedGlobal.sourceMappingURLDirective());
        if (!part.check(sameParse, makeString("decoded "_s, what, " has other global parse fields"_s)))
            return false;
    }
    if (auto* originalProgram = dynamicDowncast<UnlinkedProgramCodeBlock>(original)) {
        auto& decodedProgram = uncheckedDowncast<UnlinkedProgramCodeBlock>(decoded);
        bool sameOrder = sameDeclarationOrder(originalProgram->variableDeclarations(), decodedProgram.variableDeclarations())
            && sameDeclarationOrder(originalProgram->lexicalDeclarations(), decodedProgram.lexicalDeclarations());
        if (!part.check(sameOrder, makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalModule = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(original)) {
        auto& decodedModule = uncheckedDowncast<UnlinkedModuleProgramCodeBlock>(decoded);
        if (!part.check(sameDeclarationOrder(originalModule->variableDeclarations(), decodedModule.variableDeclarations()), makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalEval = dynamicDowncast<UnlinkedEvalCodeBlock>(original)) {
        auto& decodedEval = uncheckedDowncast<UnlinkedEvalCodeBlock>(decoded);
        bool sameNames = std::ranges::equal(originalEval->variables(), decodedEval.variables())
            && std::ranges::equal(originalEval->functionHoistingCandidates(), decodedEval.functionHoistingCandidates());
        if (!part.check(sameNames, makeString("decoded "_s, what, " has other variables or function hoisting candidates"_s)))
            return false;
    }
    const TDZEnvironmentLink* holderChain = holder ? holder->parentScopeTDZVariables().get() : nullptr;
    auto sameChild = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!child || !decodedChild)
            return false;
        auto descriptor = encodeDescriptorBytes(vm, *child);
        auto decodedDescriptor = encodeDescriptorBytes(vm, *decodedChild);
        return descriptor && decodedDescriptor && *descriptor == *decodedDescriptor && !decodedChild->features() && !decodedChild->hasCapturedVariables()
            && decodedChild->lexicallyScopedFeatures() == child->lexicallyScopedFeatures()
            && chainShape(decodedChild->parentScopeTDZVariables().get(), holderChain) == chainShape(child->parentScopeTDZVariables().get(), holderChain);
    };
    for (unsigned i = 0; i < original.numberOfFunctionDecls(); ++i) {
        if (!part.check(sameChild(original.functionDecl(i), decoded.functionDecl(i)), makeString("function declaration "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    for (unsigned i = 0; i < original.numberOfFunctionExprs(); ++i) {
        if (!part.check(sameChild(original.functionExpr(i), decoded.functionExpr(i)), makeString("function expression "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    return true;
}

// Encodes twice, decodes without and with validation, and compares each decode with the original.
static bool roundTripsCore(VM& vm, TestPart& part, UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, ASCIILiteral what)
{
    auto core = encodeCoreBytes(vm, codeBlock, holder);
    auto again = encodeCoreBytes(vm, codeBlock, holder);
    if (!part.check(core && again && *core == *again, makeString("encoding "_s, what, " twice does not give equal bytes"_s)))
        return false;
    for (bool validate : { false, true }) {
        CoreDecodeFailure failure = CoreDecodeFailure::None;
        Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, core->span(), provider, kind, holder, validate, failure) };
        if (!part.check(decoded && failure == CoreDecodeFailure::None, makeString("the core of "_s, what, validate ? " does not decode with validation"_s : " does not decode"_s)))
            return false;
        if (!sameAsDecoded(vm, part, codeBlock, *decoded.get(), holder, core->span(), what))
            return false;
    }
    return true;
}

// The corpus module, both corpus evals, every corpus program and the functions the programs declare encode
// deterministically, decode without and with validation to UCBs equal field by field, and re-encode to their cores.
static bool testCodecCorpus(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    // An import decodes every core kind, so the round trip covers each root record the codec has.
    {
        SourceCode moduleSource = makeSource(String { moduleCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
        ParserError error;
        Strong<UnlinkedModuleProgramCodeBlock> module { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, moduleSource, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
        bool hashedDeclarations = module && module.get()->variableDeclarations().mapSize() > VariableEnvironment::inlineMapCapacity;
        if (!part.check(hashedDeclarations, "the corpus module failed to generate, or its declarations fit inline storage, so the hashed layout goes untested"_s))
            return false;
        if (!part.check(!holdsSymbolKey(module.get()->variableDeclarations()), "the corpus module declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        if (!roundTripsCore(vm, part, *module.get(), nullptr, *moduleSource.provider(), UnlinkedCodeBlockCoreKind::Module, "the corpus module"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { indirectEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<IndirectEvalExecutable> indirectEval { vm, IndirectEvalExecutable::tryCreate(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, false, EvalContextType::None) };
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, indirectEval ? indirectEval.get()->unlinkedCodeBlock() : nullptr };
        bool namesDeclarations = codeBlock && codeBlock.get()->numVariables() && codeBlock.get()->numFunctionHoistingCandidates();
        if (!part.check(namesDeclarations, "the corpus indirect eval failed to generate, or lacks its variables or its hoisting candidate"_s))
            return false;
        if (!roundTripsCore(vm, part, *codeBlock.get(), nullptr, *evalSource.provider(), UnlinkedCodeBlockCoreKind::Eval, "the corpus indirect eval"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { directEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        TDZEnvironment variablesUnderTDZ = tdzNames(vm, { "jitcacheU7DirectOuter"_s });
        PrivateNameEnvironment privateNames;
        DirectEvalExecutable* directEval = DirectEvalExecutable::create(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
            PrivateBrandRequirement::None, false, true, EvalContextType::FunctionEvalContext, &variablesUnderTDZ, &privateNames);
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, directEval ? directEval->unlinkedCodeBlock() : nullptr };
        UnlinkedFunctionExecutable* function = nullptr;
        if (codeBlock) {
            function = selectChild(*codeBlock.get(), [](UnlinkedFunctionExecutable&) {
                return true;
            });
        }
        if (!part.check(function && function->parentScopeTDZVariables(), "the corpus direct eval failed to generate, or its function holds no TDZ chain"_s))
            return false;
        if (!roundTripsCore(vm, part, *codeBlock.get(), nullptr, *evalSource.provider(), UnlinkedCodeBlockCoreKind::Eval, "the corpus direct eval"_s))
            return false;
    }

    Vector<String> texts;
    for (ASCIILiteral text : codecCorpus)
        texts.append(String { text });
    texts.append(bigProgramText());
    for (const String& text : texts) {
        // Only roundTripsChild generates children, never a class field initializer's (see there).
        ProgramFixture fixture = generateProgramFixture(vm, text, 0);
        if (!part.check(!!fixture.codeBlock, "a corpus program failed to generate"_s))
            return false;
        UnlinkedProgramCodeBlock& program = *fixture.codeBlock.get();
        SourceProvider& provider = *fixture.source.provider();
        bool declaresSymbol = holdsSymbolKey(program.variableDeclarations()) || holdsSymbolKey(program.lexicalDeclarations());
        if (!part.check(!declaresSymbol, "a corpus program declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        auto roundTripsChild = [&](UnlinkedFunctionExecutable* child) {
            if (!child)
                return false;
            // A class field initializer's body is never generated here. Its parse makes one DefineFieldNode per field, a
            // parser-arena node whose destructor never runs but which holds the field's Identifier, so each field name's atom
            // leaks with JITCache off too; testjitcache has no oracle run to show that the leak is the engine's own, so the
            // test avoids input the engine leaks on. The initializer's descriptor, class element definitions included, goes
            // through the program's core.
            if (child->parseMode() == SourceParseMode::ClassFieldInitializerMode)
                return true;
            CodeSpecializationKind kind = child->isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
            ParserError error;
            Strong<UnlinkedFunctionCodeBlock> body { vm, child->unlinkedCodeBlockFor(vm, child->linkedSourceCode(fixture.source), kind, { }, error, child->parseMode()) };
            if (!body)
                return true; // a body that does not generate on its own
            return roundTripsCore(vm, part, *body.get(), child, provider, UnlinkedCodeBlockCoreKind::Function, "a corpus function"_s);
        };
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionDecls(), 8); ++i) {
            if (!roundTripsChild(program.functionDecl(i)))
                return false;
        }
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionExprs(), 8); ++i) {
            if (!roundTripsChild(program.functionExpr(i)))
                return false;
        }
        // After the children's round trips, whose generation parsed them and filled their slots: the program's decoded
        // children must still come back unparsed, with the lexically scoped features generation gave them.
        if (!roundTripsCore(vm, part, program, nullptr, provider, UnlinkedCodeBlockCoreKind::Program, "a corpus program"_s))
            return false;
    }
    return true;
}

// Counts what an encode keeps charged (signed, so a release of something never charged shows), and refuses, for good,
// from the `refuseAt`-th charge of at least a first page's size; zero never refuses.
class TestEncodingBudget final : public CoreEncodingBudget {
public:
    explicit TestEncodingBudget(unsigned refuseAt = 0)
        : m_refuseAt(refuseAt)
    {
    }

    bool charge(size_t bytes) final
    {
        if (m_refused)
            return false;
        if (!m_firstCharge)
            m_firstCharge = bytes;
        if (bytes >= firstPageSize && m_refuseAt && ++m_pageSizedCharges == m_refuseAt) {
            m_refused = true;
            return false;
        }
        m_charged += static_cast<int64_t>(bytes);
        return true;
    }

    void release(size_t bytes) final { m_charged -= static_cast<int64_t>(bytes); }

    int64_t charged() const { return m_charged; }
    bool refused() const { return m_refused; }
    size_t firstCharge() const { return m_firstCharge; }

    static constexpr size_t firstPageSize = 4096;

private:
    const unsigned m_refuseAt;
    unsigned m_pageSizedCharges { 0 };
    bool m_refused { false };
    size_t m_firstCharge { 0 };
    int64_t m_charged { 0 };
};

// The chunks of both entry points concatenate to the payloads the releasing encoders return, and the first page is 4 KiB;
// with a counting budget a payload's size stays charged; under a budget that refuses the third page every entry point
// returns BudgetRefused, the sinks see nothing and the budget is back at zero.
static bool testCodecChunksAndBudgets(VM& vm, TestPart& part)
{
    ProgramFixture big = generateProgramFixture(vm, bigProgramText(), 0);
    ProgramFixture small = generateProgramFixture(vm, "var jitcacheU7Small = 1;\n"_s, 0);
    ProgramFixture fields = generateProgramFixture(vm, manyFieldsText(), 0);
    ProgramFixture tiny = generateProgramFixture(vm, "function jitcacheU7Tiny() { }\n"_s, 0);
    if (!part.check(big.codeBlock && small.codeBlock && fields.codeBlock && tiny.codeBlock, "a chunking fixture failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable* initializer = selectChild(*fields.codeBlock.get(), [](UnlinkedFunctionExecutable& executable) {
        return executable.parseMode() == SourceParseMode::ClassFieldInitializerMode;
    });
    UnlinkedFunctionExecutable* tinyFunction = tiny.codeBlock.get()->functionDecl(0);
    if (!part.check(initializer && tinyFunction, "a chunking fixture lacks its UFE"_s))
        return false;

    auto coreChunks = [&](UnlinkedCodeBlock& codeBlock, CoreEncodingBudget* budget, Vector<size_t>& sizes, CoreEncodeFailure& failure) {
        Vector<uint8_t> bytes;
        failure = forEachUnlinkedCodeBlockCoreChunk(vm, codeBlock, nullptr, budget, [&](std::span<const uint8_t> chunk) {
            sizes.append(chunk.size());
            bytes.append(chunk);
        });
        return bytes;
    };
    auto descriptorChunks = [&](UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, Vector<size_t>& sizes, CoreEncodeFailure& failure) {
        Vector<uint8_t> bytes;
        failure = forEachUnlinkedFunctionExecutableDescriptorChunk(vm, executable, budget, [&](std::span<const uint8_t> chunk) {
            sizes.append(chunk.size());
            bytes.append(chunk);
        });
        return bytes;
    };

    for (auto* fixture : { &big, &small }) {
        UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
        auto payload = encodeCoreBytes(vm, codeBlock, nullptr);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        Vector<uint8_t> concatenated = coreChunks(codeBlock, nullptr, sizes, failure);
        bool chunked = payload && failure == CoreEncodeFailure::None && concatenated == *payload && !sizes.isEmpty() && sizes[0] <= TestEncodingBudget::firstPageSize;
        if (!part.check(chunked, "a core's chunks do not concatenate to its payload, or its first page is larger than 4 KiB"_s))
            return false;
        if (fixture == &big && !part.check(sizes.size() > 2, "the large program's core does not span three pages"_s))
            return false;
    }
    for (auto* executable : { initializer, tinyFunction }) {
        auto payload = encodeDescriptorBytes(vm, *executable);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        Vector<uint8_t> concatenated = descriptorChunks(*executable, nullptr, sizes, failure);
        bool chunked = payload && failure == CoreEncodeFailure::None && concatenated == *payload && !sizes.isEmpty() && sizes[0] <= TestEncodingBudget::firstPageSize;
        if (!part.check(chunked, "a descriptor's chunks do not concatenate to its payload, or its first page is larger than 4 KiB"_s))
            return false;
        if (executable == initializer && !part.check(sizes.size() > 2, "the large descriptor does not span three pages"_s))
            return false;
    }

    // A counting budget: the first charge is the first page, and exactly the payload's size stays charged.
    {
        TestEncodingBudget budget;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!part.check(core && budget.firstCharge() == TestEncodingBudget::firstPageSize && budget.charged() == static_cast<int64_t>(core->size()), "a core encode does not start with a 4 KiB page or does not leave its payload's size charged"_s))
            return false;
        TestEncodingBudget descriptorBudget;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &descriptorBudget, failure);
        if (!part.check(descriptor && descriptorBudget.charged() == static_cast<int64_t>(descriptor->size()), "a descriptor encode does not leave its payload's size charged"_s))
            return false;
    }

    // A budget that refuses the third page.
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!part.check(!core && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a core encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &budget, failure);
        if (!part.check(!descriptor && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a descriptor encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        coreChunks(*big.codeBlock.get(), &budget, sizes, failure);
        if (!part.check(failure == CoreEncodeFailure::BudgetRefused && sizes.isEmpty() && !budget.charged(), "the core chunks under a refusing budget reach the sink or leave a charge"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        descriptorChunks(*initializer, &budget, sizes, failure);
        if (!part.check(failure == CoreEncodeFailure::BudgetRefused && sizes.isEmpty() && !budget.charged(), "the descriptor chunks under a refusing budget reach the sink or leave a charge"_s))
            return false;
    }
    return true;
}

// Bytecode-cache payloads a test marks persistent. The blocks decoded from them borrow their bytes, so the bytes live for
// the process, as an embedder's mapped executable section does.
static Vector<Vector<uint8_t>>& persistentPayloads()
{
    static NeverDestroyed<Vector<Vector<uint8_t>>> payloads;
    return payloads.get();
}

static std::optional<uint32_t> butterflyConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<JSCellButterfly>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// What the native bytecode cache produces encodes as generation's twin would: a decoded function with plain string
// constants and a plain butterfly encodes to its generated twin's bytes and decodes with atoms; two decodes of one payload
// whose functions run in different orders encode alike; a decoded parent whose children are still cached encodes twice
// alike and leaves their decoders usable; a borrowed stream encodes as its copy.
static bool testCodecNativeDecodes(VM& vm, TestPart& part)
{
    String text = "function jitcacheU7Atoms(s) { var long = 'jitcache-u7-a-long-string-constant-that-is-no-inline-string'; return ['jitcache-u7-word-one', 'jitcache-u7-word-two'].indexOf(s) + long.length; }\n"
        "function jitcacheU7Second(s) { return 'jitcache-u7-a-long-string-constant-that-is-no-inline-string' + s + 'jitcache-u7-second-only-string'; }\n"_s;
    ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
    if (!part.check(!!generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 2, "the native-decode program failed to generate"_s))
        return false;
    SourceCodeKey key = programCacheKey(generated.source);
    RefPtr<CachedBytecode> payload = encodeCodeBlock(vm, key, generated.codeBlock.get());
    if (!part.check(!!payload, "the native bytecode cache did not encode the program"_s))
        return false;
    auto decode = [&](Ref<CachedBytecode>&& bytes) {
        return Strong<UnlinkedProgramCodeBlock> { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, WTF::move(bytes)) };
    };

    // A decoded function against its generated twin.
    auto decoded = decode(Ref<CachedBytecode> { *payload });
    if (!part.check(!!decoded, "the native bytecode cache did not decode the program"_s))
        return false;
    UnlinkedFunctionExecutable& generatedFunction = *generated.codeBlock.get()->functionDecl(0);
    UnlinkedFunctionExecutable& decodedFunction = *decoded.get()->functionDecl(0);
    Strong<UnlinkedFunctionCodeBlock> generatedBody { vm, decodedBody(vm, generatedFunction, generated.source) };
    Strong<UnlinkedFunctionCodeBlock> nativeBody { vm, decodedBody(vm, decodedFunction, generated.source) };
    if (!part.check(generatedBody && nativeBody, "a function body of the native-decode program is missing"_s))
        return false;
    auto literal = butterflyConstant(*nativeBody.get());
    bool nativeForms = !allStringConstantsAreAtoms(*nativeBody.get()) && literal && constantCell<JSCellButterfly>(*nativeBody.get(), *literal)->structure() != vm.cellButterflyOnlyAtomStringsStructure.get();
    if (!part.check(nativeForms, "the native decode made every string constant an atom or gave the literal the atom form, so the case is untested"_s))
        return false;
    auto generatedCore = encodeCoreBytes(vm, *generatedBody.get(), &generatedFunction);
    auto nativeCore = encodeCoreBytes(vm, *nativeBody.get(), &decodedFunction);
    if (!part.check(generatedCore && nativeCore && *generatedCore == *nativeCore, "a natively decoded function does not encode to its generated twin's bytes"_s))
        return false;
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> fromCore { vm, decodeCoreBytes(vm, nativeCore->span(), *generated.source.provider(), UnlinkedCodeBlockCoreKind::Function, &decodedFunction, false, failure) };
    if (!part.check(fromCore && allStringConstantsAreAtoms(*fromCore.get()), "a core decoded from a natively decoded function does not make every string constant register an atom"_s))
        return false;
    auto* nativeLiteral = constantCell<JSCellButterfly>(*nativeBody.get(), *literal);
    auto* coreLiteral = constantCell<JSCellButterfly>(*fromCore.get(), *literal);
    bool literalKeepsDecode = coreLiteral && coreLiteral->structure() == vm.cellButterflyStructure(coreLiteral->indexingMode()) && coreLiteral->length() == nativeLiteral->length();
    for (unsigned i = 0; literalKeepsDecode && i < coreLiteral->length(); ++i) {
        JSValue element = coreLiteral->get(i);
        literalKeepsDecode = element.isString() && asString(element)->tryGetValueImpl() && String { asString(element)->tryGetValueImpl() } == String { asString(nativeLiteral->get(i))->tryGetValueImpl() };
    }
    if (!part.check(literalKeepsDecode, "a decoded core's butterfly does not keep the plain structure and the string elements its decode gives"_s))
        return false;

    // Two decodes of one payload whose functions are first requested in different orders.
    auto first = decode(Ref<CachedBytecode> { *payload });
    auto second = decode(Ref<CachedBytecode> { *payload });
    if (!part.check(first && second, "a second native decode of the payload failed"_s))
        return false;
    Strong<UnlinkedFunctionCodeBlock> firstA { vm, decodedBody(vm, *first.get()->functionDecl(0), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> firstB { vm, decodedBody(vm, *first.get()->functionDecl(1), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> secondB { vm, decodedBody(vm, *second.get()->functionDecl(1), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> secondA { vm, decodedBody(vm, *second.get()->functionDecl(0), generated.source) };
    bool sameAcrossOrders = firstA && firstB && secondA && secondB
        && encodeCoreBytes(vm, *first.get(), nullptr) == encodeCoreBytes(vm, *second.get(), nullptr)
        && encodeCoreBytes(vm, *firstA.get(), first.get()->functionDecl(0)) == encodeCoreBytes(vm, *secondA.get(), second.get()->functionDecl(0))
        && encodeCoreBytes(vm, *firstB.get(), first.get()->functionDecl(1)) == encodeCoreBytes(vm, *secondB.get(), second.get()->functionDecl(1));
    if (!part.check(sameAcrossOrders, "two native decodes of one payload encode differently after their functions ran in different orders"_s))
        return false;

    // A decoded parent whose children still hold their lazily cached slots.
    auto lazy = decode(Ref<CachedBytecode> { *payload });
    auto lazyOnce = lazy ? encodeCoreBytes(vm, *lazy.get(), nullptr) : std::nullopt;
    auto lazyTwice = lazy ? encodeCoreBytes(vm, *lazy.get(), nullptr) : std::nullopt;
    if (!part.check(lazyOnce && lazyOnce == lazyTwice && lazyOnce == encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr), "a decoded parent with lazily cached children does not encode twice to its generated twin's bytes"_s))
        return false;
    Strong<UnlinkedFunctionCodeBlock> lazyChild { vm, decodedBody(vm, *lazy.get()->functionDecl(1), generated.source) };
    if (!part.check(!!lazyChild, "encoding a decoded parent spoiled a child's lazily cached slot"_s))
        return false;

    // A persistent payload: the decoded stream borrows the payload's bytes.
    Vector<Vector<uint8_t>>& payloads = persistentPayloads();
    payloads.append(Vector<uint8_t> { payload->span() });
    Ref<CachedBytecode> persistent = CachedBytecode::create(payloads.last().mutableSpan(), [](const void*) { }, { });
    persistent->setPayloadIsPersistent();
    auto borrowed = decode(WTF::move(persistent));
    if (!part.check(!!borrowed, "the native bytecode cache did not decode a persistent payload"_s))
        return false;
    if (!part.check(!Options::useBorrowedBytecodeFromCache() || borrowed.get()->instructions().isBorrowed(), "a block decoded from a persistent payload does not borrow its stream"_s))
        return false;
    return part.check(encodeCoreBytes(vm, *borrowed.get(), nullptr) == encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr), "a borrowed-stream UCB does not encode to its copied twin's bytes"_s);
}

static std::optional<uint32_t> regExpConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<RegExp>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// A RegExp constant encodes the same whatever its shared cell's compiled state, and a core decodes to the cell the VM holds
// for its pattern and flags.
static bool testCodecRegExps(VM& vm, TestPart& part)
{
    JSGlobalObject* globalObject = vm.entryScope->globalObject();
    String text = "var jitcacheU7Pattern = /jitcache-u7-(a+)b/gi;\n"_s;
    ProgramFixture generated = generateFixture(vm, text);
    if (!part.check(!!generated.codeBlock, "the RegExp program failed to generate"_s))
        return false;
    auto index = regExpConstant(*generated.codeBlock.get());
    if (!part.check(!!index, "the RegExp program holds no RegExp constant"_s))
        return false;
    RegExp* regExp = constantCell<RegExp>(*generated.codeBlock.get(), *index);
    auto encoded = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-aab"_s, 0);
    auto compiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    vm.regExpCache()->deleteAllCode();
    auto cleared = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-ab"_s, 0);
    auto recompiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    if (!part.check(encoded && encoded == compiled && encoded == cleared && encoded == recompiled, "a RegExp constant's encoding follows its cell's compiled state"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    ProgramFixture regenerated = generateFixture(vm, text);
    if (!part.check(regenerated.codeBlock && encodeCoreBytes(vm, *regenerated.codeBlock.get(), nullptr) == encoded, "a generation made while the shared cell's code was cleared encodes differently"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, encoded->span(), *generated.source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, true, failure) };
    bool sameCell = decoded && constantCell<RegExp>(*decoded.get(), *index) == regExp && encodeCoreBytes(vm, *decoded.get(), nullptr) == encoded;
    return part.check(sameCell, "a core's RegExp constant does not decode to the cell the VM holds with its code cleared, or does not re-encode to the core"_s);
}

static String moduleWithBindings(unsigned bindings)
{
    StringBuilder text;
    // The functions come first, so their own positions do not depend on how many bindings follow.
    text.append("export function jitcacheU7ModuleOuter() { return function jitcacheU7ModuleInner() { return binding0000; }; }\n"_s);
    for (unsigned i = 0; i < bindings; ++i) {
        String number = String::number(i);
        text.append("const binding"_s);
        for (unsigned digits = number.length(); digits < 4; ++digits)
            text.append('0');
        text.append(number, " = "_s, i, ";\n"_s);
    }
    return text.toString();
}

struct ModuleChainFixture {
    SourceCode source;
    Strong<UnlinkedModuleProgramCodeBlock> module;
    UnlinkedFunctionExecutable* outer { nullptr };
    Strong<UnlinkedFunctionCodeBlock> outerBody;
    UnlinkedFunctionExecutable* inner { nullptr };
    Strong<UnlinkedFunctionCodeBlock> innerBody;
};

static ModuleChainFixture generateModuleChain(VM& vm, unsigned bindings)
{
    ModuleChainFixture fixture;
    fixture.source = makeSource(moduleWithBindings(bindings), SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    ParserError error;
    fixture.module = Strong<UnlinkedModuleProgramCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, fixture.source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
    if (!fixture.module || !fixture.module.get()->numberOfFunctionDecls())
        return fixture;
    fixture.outer = fixture.module.get()->functionDecl(0);
    SourceCode outerSource = fixture.outer->linkedSourceCode(fixture.source);
    fixture.outerBody = generateBodyOf(vm, *fixture.outer, outerSource, CodeSpecializationKind::CodeForCall);
    if (!fixture.outerBody || !fixture.outerBody.get()->numberOfFunctionExprs())
        return fixture;
    fixture.inner = fixture.outerBody.get()->functionExpr(0);
    fixture.innerBody = generateBodyOf(vm, *fixture.inner, fixture.inner->linkedSourceCode(outerSource), CodeSpecializationKind::CodeForCall);
    return fixture;
}

// The names "binding" followed by four digits that a payload holds, each with its number of occurrences.
static std::optional<unsigned> countEachBindingOnce(std::span<const uint8_t> payload, unsigned bindings)
{
    static constexpr std::array<uint8_t, 7> prefix { 'b', 'i', 'n', 'd', 'i', 'n', 'g' };
    Vector<unsigned> occurrences(FillWith { }, bindings, 0);
    for (size_t i = 0; i + prefix.size() + 4 <= payload.size(); ++i) {
        if (!equalSpans(payload.subspan(i, prefix.size()), std::span<const uint8_t> { prefix }))
            continue;
        unsigned number = 0;
        bool digits = true;
        for (size_t d = 0; d < 4; ++d) {
            uint8_t character = payload[i + prefix.size() + d];
            digits &= isASCIIDigit(character);
            number = number * 10 + (character - '0');
        }
        if (!digits || number >= bindings)
            return std::nullopt;
        ++occurrences[number];
    }
    for (unsigned count : occurrences) {
        if (count != 1)
            return std::nullopt;
    }
    return bindings;
}

// The links of the chains of a UCB's children, children in table order and each chain from its child's own link, each link
// numbered where it first appears. Two UCBs share links among their children alike exactly when their numberings are equal.
// `sharedLinks` gains one for each link met again.
static Vector<unsigned> childLinkNumbering(UnlinkedCodeBlock& codeBlock, unsigned& sharedLinks)
{
    Vector<const TDZEnvironmentLink*> seen;
    Vector<unsigned> numbering;
    allChildren(codeBlock, [&](UnlinkedFunctionExecutable& child, ChildTable, uint32_t) {
        for (TDZEnvironmentLink* link = child.parentScopeTDZVariables().get(); link; link = link->parent()) {
            size_t number = seen.find(link);
            if (number == notFound) {
                number = seen.size();
                seen.append(link);
            } else
                ++sharedLinks;
            numbering.append(static_cast<unsigned>(number));
        }
        numbering.append(std::numeric_limits<unsigned>::max()); // ends the child's chain
        return true;
    });
    return numbering;
}

// Walks a generated tree and the native bytecode cache's decode of it in step, one function at a time. Each decoded child
// must hold its generated twin's TDZ chain whole, link by link (the same number of links and the same chain digest), the
// decoded children must share their links where the generated ones do, and each decoded body must encode, with its own UFE
// as the holder, to its generated twin's core. Along the way the generated child goes through each JITCacheCore operation
// that writes a chain against a holder's: its holder digest, which keeps its environments' digests, its descriptor, whose
// chain becomes a start record, and its body's core with it as the holder, whose children's chains end in a start record
// too. `multiLinkChains` counts the chains of more than one link the walk compared, and `sharedLinks` the links siblings
// share.
static bool nativeDecodeKeepsChains(VM& vm, TestPart& part, UnlinkedCodeBlock& generated, UnlinkedCodeBlock& decoded, const SourceCode& source, unsigned& multiLinkChains, unsigned& sharedLinks)
{
    bool sameTables = generated.numberOfFunctionDecls() == decoded.numberOfFunctionDecls() && generated.numberOfFunctionExprs() == decoded.numberOfFunctionExprs();
    if (!part.check(sameTables, "a UCB the native bytecode cache decoded has other child tables than its generated twin"_s))
        return false;
    unsigned decodedSharedLinks = 0;
    if (!part.check(childLinkNumbering(generated, sharedLinks) == childLinkNumbering(decoded, decodedSharedLinks), "the children of a native decode do not share their links where generation shared them"_s))
        return false;
    auto compare = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!part.check(child && decodedChild, "a child UFE is missing from a native decode or from its generated twin"_s))
            return false;
        unsigned digested = 0;
        TDZEnvironmentLink* chain = child->parentScopeTDZVariables().get();
        TDZEnvironmentLink* decodedChain = decodedChild->parentScopeTDZVariables().get();
        auto chainDigest = tdzChainDigest(chain, nullptr, digested);
        unsigned length = chainShape(chain, nullptr).length;
        bool sameChain = chainDigest && tdzChainDigest(decodedChain, nullptr, digested) == chainDigest && chainShape(decodedChain, nullptr).length == length;
        if (!part.check(sameChain, "a child the native bytecode cache decoded does not hold its generated twin's TDZ chain, link by link"_s))
            return false;
        if (length > 1)
            ++multiLinkChains;
        if (!part.check(holderDigest(vm, *child) && encodeDescriptorBytes(vm, *child), "a generated child's holder digest or descriptor failed"_s))
            return false;
        Strong<UnlinkedFunctionCodeBlock> body { vm, decodedBody(vm, *child, source) };
        Strong<UnlinkedFunctionCodeBlock> decodedChildBody { vm, decodedBody(vm, *decodedChild, source) };
        if (!part.check(body && decodedChildBody, "a body of the native round trip is missing"_s))
            return false;
        auto core = encodeCoreBytes(vm, *body.get(), child);
        if (!part.check(core && core == encodeCoreBytes(vm, *decodedChildBody.get(), decodedChild), "a function the native bytecode cache decoded does not encode, with its holder, to its generated twin's core"_s))
            return false;
        return nativeDecodeKeepsChains(vm, part, *body.get(), *decodedChildBody.get(), child->linkedSourceCode(source), multiLinkChains, sharedLinks);
    };
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i) {
        if (!compare(generated.functionDecl(i), decoded.functionDecl(i)))
            return false;
    }
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i) {
        if (!compare(generated.functionExpr(i), decoded.functionExpr(i)))
            return false;
    }
    return true;
}

// Function cores and descriptors stay the size they are whatever the enclosing module declares, and the module's own core
// holds each binding once; a chain that does not end in the holder's is written whole; decoding with the holder ends the
// children's chains in its chain object; a start record decoded without a start chain is Malformed; and the native
// bytecode cache writes and reads chains as it does without JITCache.
static bool testCodecTDZChains(VM& vm, TestPart& part)
{
    ModuleChainFixture large = generateModuleChain(vm, 1000);
    ModuleChainFixture small = generateModuleChain(vm, 10);
    if (!part.check(large.innerBody && small.innerBody, "a module of the chain test failed to generate"_s))
        return false;
    auto size = [&](const std::optional<Vector<uint8_t>>& bytes) {
        return bytes ? bytes->size() : 0;
    };
    auto largeOuterCore = encodeCoreBytes(vm, *large.outerBody.get(), large.outer);
    auto largeInnerCore = encodeCoreBytes(vm, *large.innerBody.get(), large.inner);
    bool sameSizes = largeOuterCore && size(largeOuterCore) == size(encodeCoreBytes(vm, *small.outerBody.get(), small.outer))
        && size(largeInnerCore) && size(largeInnerCore) == size(encodeCoreBytes(vm, *small.innerBody.get(), small.inner))
        && size(encodeDescriptorBytes(vm, *large.outer)) && size(encodeDescriptorBytes(vm, *large.outer)) == size(encodeDescriptorBytes(vm, *small.outer))
        && size(encodeDescriptorBytes(vm, *large.inner)) && size(encodeDescriptorBytes(vm, *large.inner)) == size(encodeDescriptorBytes(vm, *small.inner));
    if (!part.check(sameSizes, "a function core or descriptor grows with the bindings its module declares"_s))
        return false;
    auto moduleCore = encodeCoreBytes(vm, *large.module.get(), nullptr);
    if (!part.check(moduleCore && countEachBindingOnce(moduleCore->span(), 1000), "the module's own core does not hold each of its bindings exactly once"_s))
        return false;

    // Encoded with a holder whose chain is another, the children's chains are written link by link, the module's 1,000 names
    // included.
    auto foreignHolder = encodeCoreBytes(vm, *large.outerBody.get(), small.outer);
    if (!part.check(foreignHolder && size(foreignHolder) >= size(largeOuterCore) + 1000 * 11, "a chain that does not end in the holder's chain is not written whole"_s))
        return false;

    // Decoded with the holder, the children's chains end in its chain object and equal the generated children's.
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, largeOuterCore->span(), *large.source.provider(), UnlinkedCodeBlockCoreKind::Function, large.outer, true, failure) };
    if (!part.check(decoded && decoded.get()->numberOfFunctionExprs() == 1, "a function core of the chain test does not decode with its holder"_s))
        return false;
    UnlinkedFunctionExecutable& decodedInner = *decoded.get()->functionExpr(0);
    TDZEnvironmentLink* holderChain = large.outer->parentScopeTDZVariables().get();
    ChainShape decodedShape = chainShape(decodedInner.parentScopeTDZVariables().get(), holderChain);
    ChainShape generatedShape = chainShape(large.inner->parentScopeTDZVariables().get(), holderChain);
    unsigned digested = 0;
    auto decodedChain = tdzChainDigest(decodedInner.parentScopeTDZVariables().get(), nullptr, digested);
    auto generatedChain = tdzChainDigest(large.inner->parentScopeTDZVariables().get(), nullptr, digested);
    bool chainsMatch = holderChain && decodedShape.targetPosition && decodedShape == generatedShape && decodedChain && decodedChain == generatedChain;
    if (!part.check(chainsMatch, "a decoded child's chain does not end in the holder's chain object or differs from the generated child's"_s))
        return false;
    if (!part.check(encodeCoreBytes(vm, *decoded.get(), large.outer) == largeOuterCore, "decoding and re-encoding a function core with its holder does not give the core back"_s))
        return false;

    // The core's start record, decoded by a holder without a chain.
    SourceCode rootSource;
    Strong<UnlinkedFunctionExecutable> chainless = functionConstructorRoot(vm, "return 'jitcache-u7-chainless';"_s, rootSource);
    if (!part.check(chainless && !chainless.get()->parentScopeTDZVariables(), "the chainless holder has a TDZ chain"_s))
        return false;
    Strong<UnlinkedCodeBlock> withoutStartChain { vm, decodeCoreBytes(vm, largeOuterCore->span(), *large.source.provider(), UnlinkedCodeBlockCoreKind::Function, chainless.get(), false, failure) };
    if (!part.check(!withoutStartChain && failure == CoreDecodeFailure::Malformed, "a start record decoded without a start chain is not Malformed"_s))
        return false;

    // The native bytecode cache has no start chain, so it never writes a start record and its decode is unchanged: it must
    // write and read chains as it does without JITCache. No encoder without start records exists in the process to compare
    // bytes with, so the test checks what start records could have changed. A tree whose sibling functions share chains of
    // two links, and whose arrows' chains end in their functions' own chain objects, goes through the native cache and back
    // with every chain whole and every link shared where generation shared it; and the native bytes for the tree stay the
    // same after the walk has run the JITCacheCore operations on it, which turn those chains into start records and keep
    // each environment's digest.
    ProgramFixture program = generateProgramFixture(vm, String { codecCorpus[nestedScopesCorpusIndex] }, std::numeric_limits<unsigned>::max());
    if (!part.check(!!program.codeBlock, "the native round-trip program failed to generate"_s))
        return false;
    SourceCodeKey key = programCacheKey(program.source);
    RefPtr<CachedBytecode> native = encodeCodeBlock(vm, key, program.codeBlock.get());
    if (!part.check(!!native, "the native bytecode cache did not encode the round-trip program"_s))
        return false;
    Strong<UnlinkedProgramCodeBlock> nativeDecode { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, Ref<CachedBytecode> { *native }) };
    if (!part.check(!!nativeDecode, "the native bytecode cache did not decode the round-trip program"_s))
        return false;
    unsigned multiLinkChains = 0;
    unsigned sharedLinks = 0;
    if (!nativeDecodeKeepsChains(vm, part, *program.codeBlock.get(), *nativeDecode.get(), program.source, multiLinkChains, sharedLinks))
        return false;
    if (!part.check(multiLinkChains >= 2 && sharedLinks, "the native round trip compared fewer than two chains of several links, or no link that siblings share, so it cannot show the cache keeps them"_s))
        return false;
    RefPtr<CachedBytecode> nativeAgain = encodeCodeBlock(vm, key, program.codeBlock.get());
    return part.check(nativeAgain && equalSpans(native->span(), nativeAgain->span()), "the native bytecode cache writes other bytes for a tree after the JITCacheCore operations on it"_s);
}

// A UFE's descriptor is the same before and after its body is parsed and its slots filled, differs with each field it
// holds, its flags one by one included, and is the same for UFEs that differ only in their TDZ chains (the holder variants
// the holder digest test reads too).
static bool testCodecDescriptors(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "function jitcacheU7Descriptor(a, b) { return a + b; }"_s);
    if (!part.check(!!fixture.codeBlock, "the descriptor program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& executable = *fixture.codeBlock.get()->functionDecl(0);
    auto before = encodeDescriptorBytes(vm, executable);
    executable.recordParse(ArgumentsFeature, executable.lexicallyScopedFeatures(), true);
    ParserError error;
    UnlinkedFunctionCodeBlock* body = executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(fixture.source), CodeSpecializationKind::CodeForCall, { }, error, executable.parseMode());
    auto after = encodeDescriptorBytes(vm, executable);
    if (!part.check(body && before && before == after, "a descriptor changes with its UFE's own parse or the filling of its slot"_s))
        return false;
    for (const HolderVariant& variant : holderVariants) {
        HolderFixture base;
        HolderFixture other;
        if (!generateHolderPair(vm, globalObject, variant, part, base, other))
            return false;
        auto baseDescriptor = encodeDescriptorBytes(vm, *base.executable);
        auto otherDescriptor = encodeDescriptorBytes(vm, *other.executable);
        if (!part.check(baseDescriptor && otherDescriptor && ((*baseDescriptor != *otherDescriptor) == variant.descriptorDiffers), makeString("the descriptor does not follow "_s, variant.field)))
            return false;
    }
    return true;
}

static bool failsToDecodeWith(VM& vm, std::span<const uint8_t> bytes, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, bool validate, CoreDecodeFailure expected)
{
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* codeBlock = decodeCoreBytes(vm, bytes, provider, kind, nullptr, validate, failure);
    return !codeBlock && failure == expected;
}

// Each Malformed and KindMismatch condition the payload's header holds. The cases that alter a core's records are
// runCoreCodecSelfTest's, in runtime/CachedTypes.cpp beside the record types.
static bool testCodecHeader(VM& vm, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "var jitcacheU7Header = 'jitcache-u7-header';\n"_s);
    if (!part.check(!!fixture.codeBlock, "the malformed-payload program failed to generate"_s))
        return false;
    auto core = encodeCoreBytes(vm, *fixture.codeBlock.get(), nullptr);
    if (!part.check(core && core->size() > 16, "the malformed-payload program does not encode"_s))
        return false;
    SourceProvider& provider = *fixture.source.provider();
    const Vector<uint8_t>& valid = *core;
    auto altered = [&](const auto& mutate) {
        Vector<uint8_t> bytes = valid;
        mutate(bytes);
        return bytes;
    };

    uint32_t rootOffset = readField<uint32_t>(valid.span(), 8);
    Vector<uint8_t> misaligned(FillWith { }, valid.size() + 8, 0);
    memcpySpan(misaligned.mutableSpan().subspan(1, valid.size()), valid.span());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    bool misalignedFails = !decodeUnlinkedCodeBlockCore(vm, CachedBytecode::create(misaligned.mutableSpan().subspan(1, valid.size()), [](const void*) { }, { }), provider, UnlinkedCodeBlockCoreKind::Program, nullptr, false, failure)
        && failure == CoreDecodeFailure::Malformed;
    bool headerConditions = failsToDecodeWith(vm, valid.span().first(15), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && misalignedFails
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[0] ^= 0xff; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[5] = 1; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[12] = 1; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), 8, rootOffset + 1); }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), 8, static_cast<uint32_t>(bytes.size())); }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, valid.span(), provider, UnlinkedCodeBlockCoreKind::Module, false, CoreDecodeFailure::KindMismatch);
    return part.check(headerConditions, "a payload breaking a condition of its header does not decode to Malformed or KindMismatch"_s);
}

// Map.prototype.forEach holds the VM's ordered-hash-table sentinel among its constants. Its core writes the sentinel's own
// kind and decodes to this VM's sentinel, re-encoding to the core's bytes, while the native builtin cache writes the cell as
// an immutable butterfly and decodes a new butterfly in its place.
static bool testCodecSentinel(VM& vm, TestPart& part)
{
    // A builtin root of its own over forEach's text alone, as Bun creates its internal modules: the native builtin cache
    // compares its payload with the provider's whole length (decodeBuiltinFunction).
    String text = vm.builtinExecutables()->mapPrototypeForEachCodeSource().view().toString();
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> executable { vm, BuiltinExecutables::createExecutable(vm, source, Identifier::fromString(vm, "forEach"_s), ImplementationVisibility::Public,
        ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No) };
    if (!part.check(!!executable, "Map.prototype.forEach's text does not make a builtin root"_s))
        return false;
    ParserError error;
    Strong<UnlinkedFunctionCodeBlock> body { vm, executable.get()->unlinkedCodeBlockFor(vm, executable.get()->linkedSourceCode(source), CodeSpecializationKind::CodeForCall, { }, error, executable.get()->parseMode()) };
    JSCell* sentinel = vm.orderedHashTableSentinel();
    std::optional<uint32_t> index;
    for (uint32_t i = 0; body && !index && i < body.get()->constantRegisters().size(); ++i) {
        if (body.get()->constantRegisters()[i].get() == JSValue(sentinel))
            index = i;
    }
    if (!part.check(!!index, "Map.prototype.forEach's body holds no ordered-hash-table sentinel"_s))
        return false;

    auto core = encodeCoreBytes(vm, *body.get(), executable.get());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), *source.provider(), UnlinkedCodeBlockCoreKind::Function, executable.get(), true, failure) : nullptr };
    bool keepsSentinel = decoded && decoded.get()->constantRegisters()[*index].get() == JSValue(sentinel) && encodeCoreBytes(vm, *decoded.get(), executable.get()) == core;
    if (!part.check(keepsSentinel, "Map.prototype.forEach's core does not decode to this VM's sentinel, or does not re-encode to its bytes"_s))
        return false;

    // The native builtin cache: the encode holds the body the call slot keeps, and the decoded root decodes it at its first
    // request.
    RefPtr<CachedBytecode> native = encodeBuiltinFunction(vm, executable.get(), text.length(), 0);
    Strong<UnlinkedFunctionExecutable> nativeExecutable { vm, native ? decodeBuiltinFunction(vm, Ref<CachedBytecode> { *native }, *source.provider(), 0) : nullptr };
    Strong<UnlinkedFunctionCodeBlock> nativeBody { vm, nativeExecutable ? decodedBody(vm, *nativeExecutable.get(), source) : nullptr };
    JSValue nativeConstant = nativeBody && *index < nativeBody.get()->constantRegisters().size() ? nativeBody.get()->constantRegisters()[*index].get() : JSValue();
    auto* nativeButterfly = nativeConstant ? dynamicDowncast<JSCellButterfly>(nativeConstant) : nullptr;
    return part.check(nativeButterfly && nativeButterfly != sentinel && !nativeButterfly->length(), "the native builtin cache does not write the sentinel as an immutable butterfly"_s);
}

// A default class constructor, which only BuiltinExecutables::createDefaultConstructor makes, keeps
// isBuiltinDefaultClassConstructor and its constructor kind through a core, as generation left them. No holder variant can
// vary that flag alone, so the round trip checks it.
static bool testCodecDefaultConstructors(VM& vm, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "class JitcacheU7DefaultBase { }\nclass JitcacheU7DefaultDerived extends JitcacheU7DefaultBase { }\n"_s);
    if (!part.check(!!fixture.codeBlock, "the default-constructor program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& generated = *fixture.codeBlock.get();
    auto core = encodeCoreBytes(vm, generated, nullptr);
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), *fixture.source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, false, failure) : nullptr };
    bool sameTables = decoded && decoded.get()->numberOfFunctionDecls() == generated.numberOfFunctionDecls() && decoded.get()->numberOfFunctionExprs() == generated.numberOfFunctionExprs();
    if (!part.check(sameTables, "the default-constructor program's core does not decode to its child tables"_s))
        return false;
    unsigned defaultConstructors = 0;
    auto sameKind = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        defaultConstructors += child->isBuiltinDefaultClassConstructor();
        return child->isBuiltinDefaultClassConstructor() == decodedChild->isBuiltinDefaultClassConstructor() && child->constructorKind() == decodedChild->constructorKind();
    };
    bool kept = true;
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i)
        kept &= sameKind(generated.functionDecl(i), decoded.get()->functionDecl(i));
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i)
        kept &= sameKind(generated.functionExpr(i), decoded.get()->functionExpr(i));
    if (!part.check(defaultConstructors == 2, "the default-constructor program does not hold a base and a derived default constructor"_s))
        return false;
    return part.check(kept, "a default constructor decoded from a core lost isBuiltinDefaultClassConstructor or its constructor kind"_s);
}

} // namespace UCBCodecTestsInternal

using namespace UCBCodecTestsInternal;

JITCACHE_TEST(ucbCodecInlineMapTransport, Yes)
{
    TestPart part { context };
    testInlineMapTransport(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecCorpusRoundTrips, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecCorpus(vm, entered.globalObject(), part);
}

JITCACHE_TEST(ucbCodecChunksAndBudgets, Yes)
{
    TestPart part { context };
    testCodecChunksAndBudgets(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecNativeDecodes, Yes)
{
    TestPart part { context };
    testCodecNativeDecodes(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecRegExpConstants, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecRegExps(vm, part);
}

JITCACHE_TEST(ucbCodecOrderedHashTableSentinel, Yes)
{
    TestPart part { context };
    testCodecSentinel(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecTDZChains, Yes)
{
    TestPart part { context };
    testCodecTDZChains(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecDescriptors, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecDescriptors(vm, entered.globalObject(), part);
}

JITCACHE_TEST(ucbCodecDefaultConstructors, Yes)
{
    TestPart part { context };
    testCodecDefaultConstructors(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecHeaderConditions, Yes)
{
    TestPart part { context };
    testCodecHeader(*context.vm(), part);
}

// The cases that alter a core's records, which runCoreCodecSelfTest runs beside the record types in
// runtime/CachedTypes.cpp: each decodes a copy of a core with one record changed, which must fail as the case names.
JITCACHE_TEST(ucbCodecAlteredRecords, Yes)
{
    String failure;
    if (!runCoreCodecSelfTest(*context.vm(), failure))
        JITCACHE_FAIL(failure);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
