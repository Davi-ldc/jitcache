// jitcache-runs: Off --useLLInt=false
// jitcache-runs: Off --useBaselineJIT=false
// jitcache-requires: twins
// jitcache-pin: off its Off runs call jitcacheICsSnapshot, which only twins builds have, and E1 changes their baseline super_construct code

// SPEC-ics.md T2, first script: the super_construct cache of a CodeBlock born in baseline (sequence 0, LLInt off) fills
// as the LLInt fills it (sequence 1, baseline JIT off). Each derived class is constructed with skip, which returns before
// super, then with new.target A, A again and B, and the cache reads Empty, A, A and then the marker for several callees.
// Before E1 the baseline template stored back the callee it loaded, so sequence 0 stayed Empty.
load("./resources/ics.js", "caller relative");

(function main(sequence) {
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

    const jitType = ["Baseline", "LLInt"][sequence];
    check(jitType, `sequence ${sequence} is none of this script's`);

    function expectCache(Derived, opcode, state, cachedCallee, step) {
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
        new Derived(true);
        expectCache(Derived, opcode, "Empty", null, "a skip construction");
        Reflect.construct(Derived, [false, 1, 2], A);
        expectCache(Derived, opcode, "Single", A, "new.target A");
        Reflect.construct(Derived, [false, 1, 2], A);
        expectCache(Derived, opcode, "Single", A, "new.target A again");
        Reflect.construct(Derived, [false, 1, 2], B);
        expectCache(Derived, opcode, "Multiple", null, "new.target B");
    }
})(sequence());
