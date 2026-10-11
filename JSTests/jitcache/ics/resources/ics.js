// Shared by every script in JSTests/jitcache/ics/ (SPEC-ics.md section 11.2). This is the only file that names the shell
// functions twins builds register for the lane (jitcacheICsSnapshot, jitcacheDelta, jitcacheStatus) and the runner's
// argument convention: the shell's global arguments hold the role, the scratch path, the artifact path and the sequence
// index. Its top level declares functions only, so nothing it leaves reachable depends on the run's role.

// The runner's arguments, read on each call.

function role() {
    return globalThis.arguments[0];
}

function sequence() {
    return Number(globalThis.arguments[3]);
}

function scratchFile(name) {
    return `${globalThis.arguments[1]}/${name}.json`;
}

// Assertions. Each throws, which ends the run with a nonzero exit.

function fail(message) {
    throw new Error(message);
}

function check(condition, message) {
    if (!condition)
        fail(message);
}

function checkSame(actual, expected, what) {
    if (actual !== expected)
        fail(`${what} is ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`);
}

// Checks that object has each field of fields with the same value, reporting every difference at once.
function expectFields(label, object, fields) {
    check(object, `${label} does not exist`);
    const differences = [];
    for (const [field, expected] of Object.entries(fields)) {
        if (object[field] !== expected)
            differences.push(`${field} is ${JSON.stringify(object[field])}, expected ${JSON.stringify(expected)}`);
    }
    if (differences.length)
        fail(`${label}: ${differences.join("; ")}`);
}

// A caller passes what must stay reachable until this call, such as the objects whose structures a site's cases name and
// the callees a call site linked, so that no collection before delta() changes the state the snapshots read.
function keepAlive(...values) {
    return values.length;
}

// The twins shell functions.

// The executable's CodeBlock of kind "call" or "construct" (its baseline alternative for an optimized one) as
// SPEC-ics.md section 11.1 spells it, or undefined when the executable has none.
function snapshot(fn, kind = "call") {
    return jitcacheICsSnapshot(fn, kind);
}

// The full step name of the VM's first fault, or null.
function status() {
    return jitcacheStatus();
}

// Checks each body's anchor, then saves what every body learned. A body is a function or a [fn, kind] pair, and each must
// be in baseline with a property IC that lists a case, so its delta capture beats its finalize capture on the IC count at
// the latest (SPEC-ics.md section 11.2). Returns what jitcacheDelta() returns.
function delta(...bodies) {
    for (const body of bodies) {
        const [fn, kind] = Array.isArray(body) ? body : [body, "call"];
        const state = snapshot(fn, kind);
        if (!state || state.jitType !== "Baseline")
            fail(`delta: ${fn.name} (${kind}) is ${state ? state.jitType : "without a CodeBlock"}, not in baseline`);
        if (!state.propertyICs.some(ic => ic.caseCount > 0))
            fail(`delta: ${fn.name} (${kind}) has no property IC that lists a case`);
    }
    return jitcacheDelta();
}

// Bodies under test.

// Every run passes each body under test here before its first call. noDFG keeps its baseline CodeBlock the executable's
// replacement and its capability class, a baked fact, the same in every run; noInline keeps a caller's DFG compile from
// inlining it, so no compiler thread reads or drains its profiles.
function underTest(...bodies) {
    for (const body of bodies) {
        noDFG(body);
        noInline(body);
    }
}

// Calls the body with skip, through new for "construct", until its CodeBlock is in baseline. Returns its snapshot.
function toBaseline(fn, kind = "call") {
    const limit = 100000;
    for (let calls = 0; ; ++calls) {
        const state = snapshot(fn, kind);
        if (state && state.jitType === "Baseline")
            return state;
        if (calls === limit)
            fail(`${fn.name} (${kind}) is not in baseline after ${limit} calls`);
        if (kind === "construct")
            new fn(true);
        else
            fn(true);
    }
}

// Runs step(0), step(1), ... until reached() holds, at most limit times.
function drive(what, limit, step, reached) {
    for (let i = 0; i < limit; ++i) {
        step(i);
        if (reached())
            return;
    }
    fail(`${what} did not reach its state in ${limit} visits`);
}

// Runs full collections until reached() holds, at most limit times. The collector scans the stack conservatively, so a
// pointer that a returned frame left in a stack slot can keep its cell alive through a collection; each try first
// overwrites the stack below the caller. A cell that anything else keeps alive survives every try.
function collectUntil(what, reached, limit = 8) {
    for (let i = 0; i < limit; ++i) {
        overwriteStack(128);
        fullGC();
        if (reached())
            return;
    }
    fail(`${what} did not happen in ${limit} full collections`);
}

// Each frame's entry stores undefined into every one of its locals.
function overwriteStack(depth) {
    let l0, l1, l2, l3, l4, l5, l6, l7, l8, l9, l10, l11, l12, l13, l14, l15;
    if (depth)
        overwriteStack(depth - 1);
}

// Saving snapshots for a later run of the sequence. Off runs call neither.

function saveJSON(name, value) {
    check(writeFile(scratchFile(name), JSON.stringify(value)) >= 0, `cannot write ${scratchFile(name)}`);
}

function loadJSON(name) {
    return JSON.parse(readFile(scratchFile(name)));
}

// Objects that drive sites.

// count objects that each own property as a plain value property, each with a structure of its own.
function shapes(property, count, value = 1) {
    const objects = [];
    for (let i = 0; i < count; ++i) {
        const object = {};
        object[property] = value;
        object[`shape${i}`] = i;
        objects.push(object);
    }
    return objects;
}

// count objects that each own property as a JS getter, each with a structure of its own.
function getterShapes(property, count) {
    const objects = [];
    for (let i = 0; i < count; ++i) {
        const object = {};
        Object.defineProperty(object, property, { get() { return i; }, configurable: true, enumerable: true });
        object[`shape${i}`] = i;
        objects.push(object);
    }
    return objects;
}

// An uncacheable dictionary that has been flattened before, so the first caching visit that meets it as a base gives up
// (actionForCell in bytecode/Repatch.cpp).
function flattenedDictionary(property) {
    check(typeof $vm === "object", "the run has no $vm, which twins-mode runs get with --useDollarVM=true");
    const object = {};
    object[property] = 1;
    $vm.toUncacheableDictionary(object);
    $vm.flattenDictionaryObject(object);
    $vm.toUncacheableDictionary(object);
    return object;
}

// An object whose prototype holds property and is an uncacheable dictionary never flattened, so a caching visit counts
// a consideration, flattens the prototype and caches nothing (prepareChainForCaching).
function behindFreshDictionary(property) {
    check(typeof $vm === "object", "the run has no $vm, which twins-mode runs get with --useDollarVM=true");
    const prototype = {};
    prototype[property] = 1;
    const object = Object.create(prototype);
    $vm.toUncacheableDictionary(prototype);
    return object;
}

// Reading snapshots.

function expectLayout(label, state, accessTypes) {
    const actual = state.propertyICs.map(ic => ic.accessType);
    checkSame(actual.join(", "), accessTypes.join(", "), `${label}'s property ICs`);
}

function callSite(label, state, opcode, metadataID) {
    const site = state.callLinks.find(site => site.opcode === opcode && site.metadataID === metadataID);
    check(site, `${label} has no ${opcode} site ${metadataID}`);
    return site;
}

// SPEC-ics.md section 6.1 in JavaScript, written apart from the C++ derivation: the state a CodeBlock that installs an
// import has before any of its sites runs, derived from the producer's snapshot of the captured CodeBlock. Each field of
// the record is read from the snapshot field of the same name. Fields the derivation leaves as installation builds them
// take installation's value: no case, the Unset shape (no body under test reads length, whose mold starts at ArrayLength)
// and no mold carrying canBeMegamorphic.

function restoredPropertyIC(producer) {
    const givenUp = producer.holdsGaveUp && producer.caseCount === 0;
    const folded = producer.megamorphicCaseListed || producer.canBeMegamorphic;
    return {
        accessType: producer.accessType,
        bytecodeIndex: producer.bytecodeIndex,
        cacheType: "Unset",
        caseCount: 0,
        megamorphicCaseListed: false,
        holdsGaveUp: givenUp,
        canBeMegamorphic: folded && !givenUp,
        everConsidered: producer.everConsidered,
        sawNonCell: producer.sawNonCell,
        tookSlowPath: producer.tookSlowPath || givenUp,
        resetByGC: producer.resetByGC,
        countdown: 0,
        repatchCount: producer.repatchCount - Math.min(producer.repatchCount, producer.caseCount),
        numberOfCoolDowns: producer.numberOfCoolDowns,
    };
}

function restoredCallLink(producer) {
    const isVirtual = producer.mode === "Virtual";
    return {
        opcode: producer.opcode,
        metadataID: producer.metadataID,
        mode: isVirtual ? "Virtual" : "Init",
        seenOnce: !isVirtual,
        hasSeenClosure: producer.hasSeenClosure,
        clearedByGC: producer.clearedByGC,
        clearedByVirtual: producer.clearedByVirtual || isVirtual,
        maxArgumentCountIncludingThisForVarargs: producer.maxArgumentCountIncludingThisForVarargs,
        hasStub: false,
        hasLastSeenCallee: false,
    };
}

// The super_construct caches hold a cell, so every CodeBlock starts them empty, imported or not.
function restoredSuperConstruct(producer) {
    return { opcode: producer.opcode, metadataID: producer.metadataID, state: "Empty" };
}

// Checks a consumer's snapshot, taken before any of its sites runs, against the derivation of the saved producer one.
function expectRestored(label, state, saved) {
    check(state, `${label} has no CodeBlock`);
    checkSame(state.jitType, "Baseline", `${label}'s JIT type`);
    checkSame(state.propertyICs.length, saved.propertyICs.length, `${label}'s property-IC count`);
    checkSame(state.callLinks.length, saved.callLinks.length, `${label}'s call-link site count`);
    checkSame(state.superConstructs.length, saved.superConstructs.length, `${label}'s super_construct cache count`);
    saved.propertyICs.forEach((producer, index) => expectFields(`${label} property IC ${index} (${producer.accessType})`, state.propertyICs[index], restoredPropertyIC(producer)));
    saved.callLinks.forEach((producer, index) => expectFields(`${label} call-link site ${index} (${producer.opcode} ${producer.metadataID})`, state.callLinks[index], restoredCallLink(producer)));
    saved.superConstructs.forEach((producer, index) => expectFields(`${label} super_construct cache ${index}`, state.superConstructs[index], restoredSuperConstruct(producer)));
}

// Checks a snapshot, taken before any site runs, of a CodeBlock that took a parked image natively: every IC as
// installation builds it and never considered, every call-link site as linking leaves it.
function expectFresh(label, state) {
    check(state, `${label} has no CodeBlock`);
    checkSame(state.jitType, "Baseline", `${label}'s JIT type`);
    state.propertyICs.forEach((ic, index) => expectFields(`${label} property IC ${index} (${ic.accessType})`, ic, {
        cacheType: "Unset",
        caseCount: 0,
        holdsGaveUp: false,
        canBeMegamorphic: false,
        everConsidered: false,
        sawNonCell: false,
        tookSlowPath: false,
        resetByGC: false,
        repatchCount: 0,
        numberOfCoolDowns: 0,
    }));
    state.callLinks.forEach((site, index) => expectFields(`${label} call-link site ${index} (${site.opcode} ${site.metadataID})`, site, {
        mode: "Init",
        seenOnce: false,
        hasSeenClosure: false,
        clearedByGC: false,
        clearedByVirtual: false,
        maxArgumentCountIncludingThisForVarargs: 0,
        hasStub: false,
        hasLastSeenCallee: false,
    }));
}
