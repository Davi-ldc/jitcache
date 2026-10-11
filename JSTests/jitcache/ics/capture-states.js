// jitcache-runs: Producer --useConcurrentGC=false; Consumer
// jitcache-requires: twins

// SPEC-ics.md T3. The producer drives property ICs and call-link sites into every state SPEC-ics.md section 5 names, checks
// the states it reached, saves each body's snapshot and passes every body to delta(). Each body's first property IC lists a
// case when delta() runs and anchors it. The consumer creates each body's CodeBlock with one skip call and checks that its
// snapshot is what section 6.1 derives from the saved one; the twin check of every import covers I3 to I5 beside it.
load("./resources/ics.js", "caller relative");

(function main(role) {
    // Property ICs of get_by_id, one per state. Case n reads one name, so the IC of case n is IC n.
    function gets(skip, op, o) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            return o.cold;
        case 2:
            return o.considered;
        case 3:
            return o.three;
        case 4:
            return o.folded;
        case 5:
            return o.getter;
        case 6:
            return o.foldedGaveUp;
        case 7:
            return o.dictionary;
        case 8:
            return o.beside;
        case 9:
            return o.nonCell;
        case 10:
            return o.reset;
        case 11:
            return o.coolDown;
        }
    }
    const getsSite = { anchor: 0, cold: 1, considered: 2, three: 3, folded: 4, getter: 5, foldedGaveUp: 6, dictionary: 7, beside: 8, nonCell: 9, reset: 10, coolDown: 11 };
    const getsLayout = Array(12).fill("GetById");

    // The other access types: the folds of put, in, get by value, put by value, in by value and instanceof, and a direct
    // put that gives up on its eighth case. instanceof also loads Symbol.hasInstance and prototype through two get_by_id
    // ICs ahead of its own. The object spread calls the cloneObject link-time constant, an op_call site, before the
    // direct put of the property after it.
    function others(skip, op, o, k, v) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            o.stored = v;
            return;
        case 2:
            return "held" in o;
        case 3:
            return o[k];
        case 4:
            o[k] = v;
            return;
        case 5:
            return k in o;
        case 6:
            return o instanceof v;
        case 7:
            return { ...o, direct: v };
        }
    }
    const othersOp = { anchor: 0, put: 1, in: 2, getByVal: 3, putByVal: 4, inByVal: 5, instanceOf: 6, direct: 7 };
    const othersIC = { anchor: 0, put: 1, in: 2, getByVal: 3, putByVal: 4, inByVal: 5, instanceOf: 8, direct: 9 };
    const othersLayout = ["GetById", "PutByIdSloppy", "InById", "GetByVal", "PutByValSloppy", "InByVal", "GetById", "GetById", "InstanceOf", "PutByIdDirectSloppy"];

    // A body without a metadata table: in_by_id, a branch and returns carry no metadata entry. Its one IC anchors it.
    function noTable(skip, o) {
        if (skip)
            return;
        return "inTable" in o;
    }

    // Call-link sites: six op_call sites, a construct and a varargs call. Case n + 1 holds op_call site n.
    function calls(skip, op, o, f, args) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            return f();
        case 2:
            return f();
        case 3:
            return f();
        case 4:
            return f();
        case 5:
            return f();
        case 6:
            return f();
        case 7:
            return new f();
        case 8:
            return f(...args);
        }
    }
    const callSiteOf = { never: 0, once: 1, monomorphic: 2, polymorphic: 3, closures: 4, collected: 5 };
    const constructOp = 7;
    const varargsOp = 8;

    // In strict code the call in return position is a tail call.
    function tailCalls(skip, o, f) {
        "use strict";
        if (skip)
            return;
        if (f === undefined)
            return o.anchor;
        return f(o);
    }

    function evalCaller(skip, o, code) {
        if (skip)
            return;
        if (code === undefined)
            return o.anchor;
        return eval(code);
    }

    const bodies = { gets, others, noTable, calls, tailCalls, evalCaller };

    function first() {
        return 1;
    }
    function second() {
        return 2;
    }
    function makeClosure(value) {
        return function closure() {
            return value;
        };
    }
    function Built() {
    }
    function Other() {
    }
    function spreadTarget() {
        return 0;
    }
    function tailTarget(o) {
        return o.anchor;
    }
    function Instance() {
    }

    // The two states a full collection makes. Each helper makes, uses and drops its cells in a frame of its own and
    // checks its site while it still holds the cell the site names, so that afterwards only stale stack slots can keep
    // those cells alive. The victim's structure must also escape two engine paths that mark it without the victim: a
    // collection marks a case's structure when the structure's realm and prototype are marked by the time it visits the
    // case's CodeBlock (AccessCase::propagateTransitions calls Structure::markIfCheap), and cacheVictim's cached
    // transition that adds reset marks its new structure when its old one is marked (CodeBlock::propagateTransitions).
    // An object literal is exposed to both, since every collection marks Object.prototype and an allocation profile
    // holds the literal's first structure. So the victim's prototype is a fresh object that only its structures
    // reference, and its first structure comes from the realm's structure cache, which holds it weakly.
    function cacheVictim() {
        const victim = Object.create({});
        victim.reset = 1;
        gets(false, getsSite.reset, victim);
        gets(false, getsSite.reset, victim);
        checkSame(snapshot(gets).propertyICs[getsSite.reset].caseCount, 1, "the reset site's cases before the collection");
    }
    function linkCollectable(anchor) {
        const collectable = new Function("return 3;");
        calls(false, callSiteOf.collected + 1, anchor, collectable);
        calls(false, callSiteOf.collected + 1, anchor, collectable);
        checkSame(callSite("calls", snapshot(calls), "op_call", callSiteOf.collected).mode, "Monomorphic", "the collected callee's site before the collection");
    }

    function produce() {
        underTest(...Object.values(bodies));
        for (const body of Object.values(bodies))
            toBaseline(body);

        const anchor = { anchor: 1 };
        const alive = [anchor, first, second, Built, Other, spreadTarget, tailTarget, Instance];
        const icOf = (body, index) => snapshot(body).propertyICs[index];

        // The collections first, while no other site holds a case that one could reset.
        cacheVictim();
        linkCollectable(anchor);
        collectUntil("the victim's death and the collectable callee's", () => {
            const collected = callSite("calls", snapshot(calls), "op_call", callSiteOf.collected);
            // A function that died while its executable lived marks its site hasSeenClosure, which no later collection
            // undoes, so the checks below fail on it.
            const unlinked = collected.clearedByGC || collected.hasSeenClosure;
            return icOf(gets, getsSite.reset).resetByGC && unlinked;
        });

        for (const body of [gets, others, calls]) {
            body(false, 0, anchor);
            body(false, 0, anchor);
        }
        tailCalls(false, anchor);
        tailCalls(false, anchor);
        evalCaller(false, anchor);
        evalCaller(false, anchor);

        // gets.
        gets(false, getsSite.considered, { considered: 1 });
        const three = shapes("three", 3);
        alive.push(three);
        for (const o of [three[0], three[0], three[1], three[2]])
            gets(false, getsSite.three, o);
        const folded = shapes("folded", 8);
        alive.push(folded);
        drive("the folded get", 24, i => gets(false, getsSite.folded, folded[i % 8]), () => icOf(gets, getsSite.folded).megamorphicCaseListed);
        const getters = getterShapes("getter", 8);
        alive.push(getters);
        drive("the getter fold", 24, i => gets(false, getsSite.getter, getters[i % 8]), () => icOf(gets, getsSite.getter).megamorphicCaseListed);
        const foldedGaveUp = shapes("foldedGaveUp", 8);
        alive.push(foldedGaveUp);
        drive("the get that folds and then gives up", 24, i => gets(false, getsSite.foldedGaveUp, foldedGaveUp[i % 8]), () => icOf(gets, getsSite.foldedGaveUp).megamorphicCaseListed);
        // A number base sends the folded site's megamorphic operation to repatchGetBySlowPathCall.
        gets(false, getsSite.foldedGaveUp, 0);
        const dictionary = flattenedDictionary("dictionary");
        alive.push(dictionary);
        drive("the immediate give-up", 4, () => gets(false, getsSite.dictionary, dictionary), () => icOf(gets, getsSite.dictionary).holdsGaveUp);
        const beside = shapes("beside", 2);
        alive.push(beside);
        for (const o of [beside[0], beside[0], beside[1]])
            gets(false, getsSite.beside, o);
        const besideDictionary = flattenedDictionary("beside");
        alive.push(besideDictionary);
        gets(false, getsSite.beside, besideDictionary);
        gets(false, getsSite.nonCell, 0);
        drive("the cool-down", 30, () => gets(false, getsSite.coolDown, behindFreshDictionary("coolDown")), () => icOf(gets, getsSite.coolDown).numberOfCoolDowns === 1);

        // others.
        const folds = [
            ["put", "stored", (o, key) => others(false, othersOp.put, o, key, 2)],
            ["in", "held", (o, key) => others(false, othersOp.in, o)],
            ["getByVal", "byVal", (o, key) => others(false, othersOp.getByVal, o, key)],
            ["putByVal", "byValStore", (o, key) => others(false, othersOp.putByVal, o, key, 2)],
            ["inByVal", "byValIn", (o, key) => others(false, othersOp.inByVal, o, key)],
            ["instanceOf", "instance", (o, key) => others(false, othersOp.instanceOf, o, undefined, Instance)],
        ];
        for (const [site, key, visit] of folds) {
            const objects = shapes(key, 8);
            alive.push(objects);
            drive(`the ${site} fold`, 24, i => visit(objects[i % 8], key), () => icOf(others, othersIC[site]).megamorphicCaseListed);
        }
        // Each case of the direct put is a transition whose new structure only the spread's result holds.
        const spread = shapes("spread", 8);
        const spreadResults = [];
        alive.push(spread, spreadResults);
        drive("the direct put", 24, i => spreadResults.push(others(false, othersOp.direct, spread[i % 8], undefined, 3)), () => icOf(others, othersIC.direct).holdsGaveUp);

        // noTable: three shapes.
        const inTable = shapes("inTable", 3);
        alive.push(inTable);
        for (const o of [inTable[0], inTable[0], inTable[1], inTable[2]])
            noTable(false, o);

        // calls. The never-called site stays as linking left it.
        const callTo = (site, f) => calls(false, callSiteOf[site] + 1, anchor, f);
        callTo("once", first);
        callTo("monomorphic", first);
        callTo("monomorphic", first);
        callTo("polymorphic", first);
        callTo("polymorphic", first);
        callTo("polymorphic", second);
        const closures = [makeClosure(1), makeClosure(2)];
        alive.push(closures);
        callTo("closures", closures[0]);
        callTo("closures", closures[0]);
        callTo("closures", closures[1]);
        calls(false, constructOp, anchor, Built);
        calls(false, constructOp, anchor, Built);
        calls(false, constructOp, anchor, Other);
        for (const args of [[1], [1, 2, 3], [1, 2, 3, 4, 5]])
            calls(false, varargsOp, anchor, spreadTarget, args);

        tailCalls(false, anchor, tailTarget);
        tailCalls(false, anchor, tailTarget);
        evalCaller(false, anchor, "0");

        // Every body's snapshot, read with no body run in between, its checks, then the save and delta.
        const saved = {};
        for (const [name, body] of Object.entries(bodies))
            saved[name] = snapshot(body);

        expectLayout("gets", saved.gets, getsLayout);
        const getIC = saved.gets.propertyICs;
        expectFields("gets anchor", getIC[getsSite.anchor], { caseCount: 1 });
        expectFields("gets cold", getIC[getsSite.cold], { everConsidered: false, caseCount: 0 });
        expectFields("gets considered", getIC[getsSite.considered], { everConsidered: true, caseCount: 0 });
        expectFields("gets three", getIC[getsSite.three], { caseCount: 3, megamorphicCaseListed: false });
        expectFields("gets folded", getIC[getsSite.folded], { caseCount: 1, megamorphicCaseListed: true });
        expectFields("gets getter", getIC[getsSite.getter], { caseCount: 1, megamorphicCaseListed: true });
        expectFields("gets foldedGaveUp", getIC[getsSite.foldedGaveUp], { caseCount: 0, megamorphicCaseListed: false, cacheType: "Stub" });
        expectFields("gets dictionary", getIC[getsSite.dictionary], { caseCount: 0 });
        expectFields("gets beside", getIC[getsSite.beside], { caseCount: 2 });
        expectFields("gets nonCell", getIC[getsSite.nonCell], { sawNonCell: true, everConsidered: false });
        expectFields("gets reset", getIC[getsSite.reset], { resetByGC: true, caseCount: 0, cacheType: "Unset" });
        expectFields("gets coolDown", getIC[getsSite.coolDown], { numberOfCoolDowns: 1, caseCount: 0 });

        expectLayout("others", saved.others, othersLayout);
        const otherIC = saved.others.propertyICs;
        expectFields("others anchor", otherIC[othersIC.anchor], { caseCount: 1 });
        for (const site of ["put", "in", "getByVal", "putByVal", "inByVal", "instanceOf"])
            expectFields(`others ${site}`, otherIC[othersIC[site]], { caseCount: 1, megamorphicCaseListed: true });
        expectFields("others direct", otherIC[othersIC.direct], { caseCount: 8, megamorphicCaseListed: false });
        callSite("others", saved.others, "op_call", 0);

        expectLayout("noTable", saved.noTable, ["InById"]);
        expectFields("noTable in", saved.noTable.propertyICs[0], { caseCount: 3 });
        checkSame(saved.noTable.callLinks.length, 0, "noTable's call-link site count");

        expectLayout("calls", saved.calls, ["GetById"]);
        expectFields("calls anchor", saved.calls.propertyICs[0], { caseCount: 1 });
        checkSame(saved.calls.callLinks.map(site => site.opcode).join(", "), [...Array(6).fill("op_call"), "op_construct", "op_call_varargs"].join(", "), "calls' call-link sites");
        const callIn = site => callSite("calls", saved.calls, "op_call", callSiteOf[site]);
        expectFields("calls never", callIn("never"), { mode: "Init", seenOnce: false });
        expectFields("calls once", callIn("once"), { mode: "Init", seenOnce: true });
        expectFields("calls monomorphic", callIn("monomorphic"), { mode: "Monomorphic" });
        expectFields("calls polymorphic", callIn("polymorphic"), { mode: "Polymorphic", hasSeenClosure: false });
        expectFields("calls closures", callIn("closures"), { mode: "Polymorphic", hasSeenClosure: true });
        // A collected closure whose executable outlived it would give hasSeenClosure here and leave E2 unexercised.
        expectFields("calls collected", callIn("collected"), { mode: "Init", seenOnce: false, clearedByGC: true, hasSeenClosure: false });
        expectFields("calls construct", callSite("calls", saved.calls, "op_construct", 0), { mode: "Virtual", clearedByVirtual: true });
        expectFields("calls varargs", callSite("calls", saved.calls, "op_call_varargs", 0), { mode: "Monomorphic", maxArgumentCountIncludingThisForVarargs: 6 });

        expectLayout("tailCalls", saved.tailCalls, ["GetById"]);
        expectFields("tailCalls tail call", callSite("tailCalls", saved.tailCalls, "op_tail_call", 0), { mode: "Monomorphic" });
        expectLayout("evalCaller", saved.evalCaller, ["GetById"]);
        callSite("evalCaller", saved.evalCaller, "op_call_direct_eval", 0);

        // holdsGaveUp marks exactly the sites driven onto their *GaveUp operation, which checks those access types' E3
        // entries against the engine.
        const gaveUp = new Set([`gets ${getsSite.foldedGaveUp}`, `gets ${getsSite.dictionary}`, `gets ${getsSite.beside}`, `others ${othersIC.instanceOf}`, `others ${othersIC.direct}`]);
        for (const [name, state] of Object.entries(saved)) {
            state.propertyICs.forEach((ic, index) => {
                const site = `${name} ${index}`;
                checkSame(ic.holdsGaveUp, gaveUp.has(site), `holdsGaveUp at ${site} (${ic.accessType})`);
            });
        }

        saveJSON("capture-states", saved);
        delta(...Object.values(bodies));
        keepAlive(alive);
    }

    function consume(checking) {
        underTest(...Object.values(bodies));
        for (const body of Object.values(bodies))
            body(true);
        if (!checking)
            return;
        const saved = loadJSON("capture-states");
        for (const [name, body] of Object.entries(bodies))
            expectRestored(name, snapshot(body), saved[name]);
    }

    switch (role) {
    case "Producer":
        produce();
        break;
    case "Consumer":
        consume(true);
        break;
    case "Off":
        consume(false);
        break;
    default:
        fail(`unknown role ${role}`);
    }
})(role());
