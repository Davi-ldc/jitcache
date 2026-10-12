// SPEC-ucb.md section 13.3, map-set-foreach.js, F27 and SPEC-ucb.codec.md E16: Map.prototype.forEach and
// Set.prototype.forEach hold the VM's ordered-hash-table sentinel as a constant and end their loop when the table
// iteration returns it, comparing pointers. The Producer runs both until they reach the baseline, where they are
// captured; in the Consumer each body imports at its first call (statistics, imports), which is made over an empty
// collection, whose storage is the sentinel itself, and then both run over non-empty ones and over a map and a set that
// the callbacks shrink while they iterate. T6 checks at each import that the constant is this VM's sentinel (section
// 13.2); an import that held a copy would never leave the loop.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const rounds = 400;

    function firstCall(label, call)
    {
        const before = t.statistics();
        call();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), { imports: [">=", 1] });
    }

    let mapTotal = 0;
    let setTotal = 0;
    const addMapEntry = (value, key) => {
        mapTotal = (mapTotal + value * key.length) | 0;
    };
    const addSetEntry = value => {
        setTotal = (setTotal + value) | 0;
    };

    firstCall("Map.prototype.forEach over an empty map", () => new Map().forEach(addMapEntry));
    firstCall("Set.prototype.forEach over an empty set", () => new Set().forEach(addSetEntry));

    const map = new Map([["one", 1], ["two", 2], ["three", 3], ["four", 4]]);
    const set = new Set([5, 7, 11, 13]);
    for (let i = 0; i < rounds; ++i) {
        map.forEach(addMapEntry);
        set.forEach(addSetEntry);
        new Map().forEach(addMapEntry);
        new Set().forEach(addSetEntry);
    }
    print(`whole collections: ${mapTotal} ${setTotal}`);

    // Entries deleted ahead of the iteration and added behind it, so that the iteration reaches the table's end through
    // a table the callback changed.
    const visited = [];
    const shrinking = new Map([["a", 1], ["b", 2], ["c", 3], ["d", 4], ["e", 5]]);
    shrinking.forEach((value, key, owner) => {
        visited.push(key);
        owner.delete(String.fromCharCode(key.charCodeAt(0) + 1));
        if (value === 1)
            owner.set("z", 26);
    });
    const shrinkingSet = new Set([1, 2, 3, 4, 5, 6]);
    shrinkingSet.forEach(value => {
        visited.push(value);
        shrinkingSet.delete(value + 1);
    });
    print(`changed while iterating: ${visited.join(" ")}, left ${[...shrinking.keys()].join("")} and ${[...shrinkingSet].join("")}`);
    t.finish();
})(...arguments);
