#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "Identifier.h"
#include "JSCInlines.h"
#include "VariableEnvironmentInlines.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wtf/InlineMap.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The test of the InlineMap transport, which the UCB lane's core codec in runtime/CachedTypes.cpp uses to keep the layout
// of a hashed declaration map.

namespace JSC::JITCache::Tests {

namespace UCBCodecInlineMapTestsInternal {

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
    // Load, minimum load and the inline capacity, from layouts whose keys each sit where their probe starts, so that nothing
    // else is wrong with them: one with the same occupancy restores, while thirteen live buckets in sixteen pass the most a
    // map built by add() holds, 128 buckets for eleven entries pass minLoadInverse (6) times the entries, which a removal
    // would shrink, and eight buckets for three entries meet both loads but are no more than the nine the map keeps inline.
    auto loadControl = directLayout(keys, 1000, 64, 11);
    auto crowded = directLayout(keys, 1000, 16, 13);
    auto sparse = directLayout(keys, 1000, 128, 11);
    auto inlineSized = directLayout(keys, 1000, 8, 3);
    if (!part.check(loadControl && crowded && sparse && inlineSized, "the load test layouts could not be built from the test keys"_s))
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
        && refuses("a capacity no larger than the inline capacity"_s, inlineSized->capacity, inlineSized->slots, inlineSized->entries)
        && refuses("slot indices out of order"_s, base.capacity, swapped, base.entries)
        && refuses("a slot index at the capacity"_s, base.capacity, pastCapacity, base.entries)
        && refuses("fewer entries than live slots"_s, base.capacity, base.slots, fewer)
        && refuses("a duplicated key"_s, base.capacity, base.slots, duplicated)
        && refuses("an empty key"_s, base.capacity, base.slots, nullKey)
        && refuses("an entry its probe sequence cannot reach"_s, base.capacity, movedSlots, movedEntries)
        && refuses("more live buckets than the load factor allows"_s, crowded->capacity, crowded->slots, crowded->entries)
        && refuses("a capacity above minLoadInverse times the entries"_s, sparse->capacity, sparse->slots, sparse->entries);
}

} // namespace UCBCodecInlineMapTestsInternal

using namespace UCBCodecInlineMapTestsInternal;

JITCACHE_TEST(ucbCodecInlineMapTransport, Yes)
{
    TestPart part { context };
    testInlineMapTransport(*context.vm(), part);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
