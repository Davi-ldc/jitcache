//@ requireOptions("--useLLInt=false")

// With the LLInt off, every CodeBlock is born in baseline, so only baseline code fills the new.target
// cache of super_construct and super_construct_varargs. Each wave constructs fresh classes and drops
// them; the collection after it must clear a cache that still holds a dead class before any code reads
// it again. The last waves move both caches to SeenMultipleCalleeObjects, which collections keep.

function assert(condition, message) {
    if (!condition)
        throw new Error("Bad assertion: " + message);
}

class Base {
    constructor(a = 0, b = 0) {
        this.newTarget = new.target;
        this.total = a + b;
    }
}

// Every call returns new classes whose constructors share one executable per class, so the waves
// fill the same two caches.
function makeClasses() {
    class Plain extends Base {
        constructor(a, b) {
            super();
            this.a = a;
            this.b = b;
        }
    }
    class Spread extends Base {
        constructor(...args) {
            super(...args);
            this.count = args.length;
        }
    }
    return { Plain, Spread };
}

function checkObject(object, newTarget) {
    assert(object.newTarget === newTarget, "new.target");
    assert(Object.getPrototypeOf(object) === newTarget.prototype, "prototype");
}

function checkPlain(object, newTarget, a, b) {
    checkObject(object, newTarget);
    assert(object.total === 0, "Plain total");
    assert(object.a === a && object.b === b, "Plain fields");
}

function checkSpread(object, newTarget, a, b) {
    checkObject(object, newTarget);
    assert(object.total === a + b, "Spread total");
    assert(object.count === 2, "Spread count");
}

// One new.target per cache: each cache holds this wave's class, which dies after the wave.
function runSingleTargetWave(wave, iterations) {
    let { Plain, Spread } = makeClasses();
    for (let i = 0; i < iterations; ++i) {
        checkPlain(new Plain(i, wave), Plain, i, wave);
        checkSpread(new Spread(i, wave), Spread, i, wave);
    }
}

// Three new.targets per cache: the class itself, a subclass whose default constructor calls it, and
// an unrelated constructor through Reflect.construct.
function runMultipleTargetWave(wave, iterations) {
    let { Plain, Spread } = makeClasses();
    class PlainChild extends Plain { }
    class SpreadChild extends Spread { }
    function Other() { }
    for (let i = 0; i < iterations; ++i) {
        switch (i % 3) {
        case 0:
            checkPlain(new Plain(i, wave), Plain, i, wave);
            checkSpread(new Spread(i, wave), Spread, i, wave);
            break;
        case 1:
            checkPlain(new PlainChild(i, wave), PlainChild, i, wave);
            checkSpread(new SpreadChild(i, wave), SpreadChild, i, wave);
            break;
        case 2:
            checkPlain(Reflect.construct(Plain, [i, wave], Other), Other, i, wave);
            checkSpread(Reflect.construct(Spread, [i, wave], Other), Other, i, wave);
            break;
        }
    }
}

const singleTargetWaves = 6;
const iterations = Math.ceil(testLoopCount / singleTargetWaves);
let wave = 0;
for (; wave < singleTargetWaves; ++wave) {
    runSingleTargetWave(wave, iterations);
    if (wave & 1)
        fullGC();
    else
        edenGC();
}
runMultipleTargetWave(wave++, iterations);
fullGC();
runSingleTargetWave(wave++, iterations);
edenGC();
runMultipleTargetWave(wave++, iterations);
