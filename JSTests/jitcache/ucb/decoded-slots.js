// jitcache-runs: Off --diskCachePath=.; Producer --diskCachePath=. --jitcache-delta-at-exit; Consumer --diskCachePath=.
// jitcache-pin: off its runs decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and generates where this build decodes

// SPEC-ucb.md section 13.3, decoded-slots.js: a program whose provider carries cached bytecode with nested function
// bodies, decoded in the Producer and in the Consumer from the jsc shell's bytecode cache (--diskCachePath; the Bun
// variant is decoded-slots-bun.js). The first run, with JITCache off, writes the cache: it calls DsF and DsG and constructs
// them, so both their slots are cached, and only calls DsH, so DsH's construct slot is not. In the Consumer:
// - the requested slot of each lazily decoded function is seeded at its first call, and DsF's child, which keeps its
//   lazily cached slot, is seeded at its own (statistics, section 7.3.3); the slot the call did not ask for is recorded
//   as decoded (section 7.3.5);
// - DsF's construct slot, whose only string constant is an inline string and so decodes as an atom (SPEC-ucb.codec.md,
//   E10), attaches at its own first request (section 7.3.4);
// - DsG's construct slot holds a longer string constant, which its body uses as a property key: at its first request
//   it misses AtomMap without a stamp, its native run makes the constant an atom, and in a second realm, at a later
//   request, it attaches without computing a digest (I21);
// - `new DsH()` first records DsH's decoded call slot without settling the request, and its empty construct slot
//   imports;
// - T4 holds at every seeding (section 13.2).
// The first run tells itself from the oracle's Off runs by a file it leaves in the scratch directory.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const writesCache = t.off && !t.hasFile("decoded-slots.cache-written");

    const program = `
        function DsF(x) {
            function dsChild(y) { return y + 1; }
            if (new.target) {
                this.tag = "ab";
                this.value = x;
                return;
            }
            return dsChild(x) * 2;
        }
        function DsG(x) {
            if (new.target) {
                const key = "decodedSlotsLongPropertyKey";
                this[key] = x;
                return;
            }
            return x * 3;
        }
        function DsH(x) {
            if (new.target) {
                this.h = x;
                return;
            }
            return x - 1;
        }
    `;

    if (writesCache) {
        t.writeText("decoded-slots.cache-written", "the first run wrote the bytecode cache");
        const realm = runString(program);
        realm.DsF(1);
        new realm.DsF(1);
        realm.DsG(1);
        new realm.DsG(1);
        realm.DsH(1);
        print("the first run wrote the bytecode cache");
        return;
    }

    function window(label, action, expectations)
    {
        const before = t.statistics();
        const value = action();
        const after = t.statistics();
        if (t.role === "Consumer")
            t.expect(label, t.delta(before, after), expectations);
        return value;
    }

    const first = runString(program);
    const results = [];
    results.push(window("DsF's first call", () => first.DsF(1), { seededDecodes: 2, "records.Decoded": [">=", 3] }));
    results.push(window("DsF's first construction", () => new first.DsF(2), { attaches: 1 }).value);
    results.push(window("DsG's first call", () => first.DsG(1), { seededDecodes: 1, "records.Decoded": [">=", 2] }));
    results.push(window("DsG's first construction", () => new first.DsG(2), { attaches: 0, "misses.AtomMap": 1 }).decodedSlotsLongPropertyKey);
    results.push(window("DsH's first construction", () => new first.DsH(3), { "records.Decoded": [">=", 1], imports: 1 }).h);

    const second = runString(program);
    results.push(window("DsG's construction in a second realm", () => new second.DsG(4), { attaches: 1, contextDigests: 0, holderDigests: 0, sourceDigests: 0 }).decodedSlotsLongPropertyKey);

    let total = 0;
    for (const realm of [first, second]) {
        for (let i = 0; i < 400; ++i) {
            total = (total + realm.DsF(i) + new realm.DsF(i).value) | 0;
            total = (total + realm.DsG(i) + new realm.DsG(i).decodedSlotsLongPropertyKey) | 0;
            total = (total + realm.DsH(i) + new realm.DsH(i).h) | 0;
        }
    }
    print(`${results.join(" ")} ${total}`);
    t.finish();
})(...arguments);
