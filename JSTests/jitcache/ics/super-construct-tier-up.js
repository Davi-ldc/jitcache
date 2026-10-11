// jitcache-runs: Off
// jitcache-requires: twins
// jitcache-pin: off its Off run calls jitcacheICsSnapshot, which only twins builds have, and E1 changes its baseline super_construct code

// SPEC-ics.md T2, second script: a CodeBlock that tiers up from the LLInt keeps updating its super_construct cache in
// baseline code. The LLInt stores new.target A; skip constructions, which return before super, bring the CodeBlock to
// baseline without touching the cache; construction with B in baseline code then leaves the marker for several callees.
// Before E1 the baseline template stored back the A it loaded.
load("./resources/ics.js", "caller relative");

(function main() {
    class Base {
    }

    class Direct extends Base {
        constructor(skip) {
            if (skip)
                return {};
            super();
        }
    }

    class Spread extends Base {
        constructor(skip, ...rest) {
            if (skip)
                return {};
            super(...rest);
        }
    }

    function A() {
    }

    function B() {
    }

    function expectCache(Derived, opcode, jitType, state, cachedCallee, step) {
        const label = `${Derived.name} after ${step}`;
        const snapshotOfDerived = snapshot(Derived, "construct");
        check(snapshotOfDerived, `${label} has no construct CodeBlock`);
        checkSame(snapshotOfDerived.jitType, jitType, `${label}: the JIT type`);
        checkSame(snapshotOfDerived.superConstructs.length, 1, `${label}: the number of super_construct caches`);
        const cache = snapshotOfDerived.superConstructs[0];
        checkSame(cache.opcode, opcode, `${label}: the cache's opcode`);
        checkSame(cache.state, state, `${label}: the cache's state`);
        checkSame(cache.cachedCallee, cachedCallee, `${label}: the cached callee`);
    }

    for (const [Derived, opcode] of [[Direct, "op_super_construct"], [Spread, "op_super_construct_varargs"]]) {
        underTest(Derived);
        Reflect.construct(Derived, [false, 1, 2], A);
        expectCache(Derived, opcode, "LLInt", "Single", A, "new.target A in the LLInt");
        toBaseline(Derived, "construct");
        expectCache(Derived, opcode, "Baseline", "Single", A, "tier-up by skip constructions");
        Reflect.construct(Derived, [false, 1, 2], B);
        expectCache(Derived, opcode, "Baseline", "Multiple", null, "new.target B in baseline code");
    }
})();
