// jitcache-runs: Producer --useConcurrentGC=false; Consumer
// jitcache-requires: twins

// What an imported site does at its first consumer visit. The producer folds a get, a put, an in and an
// instanceof site, gives one site up with no case listed, makes a get_by_id of name, a name the megamorphic fold
// refuses, give up on its eighth case, leaves one site unreached, links a call site, calls a second call site once,
// leaves a third call site and a varargs site never called, runs a tail call once and sends a construct site virtual,
// each body holding an anchor (its first property IC, with one case). After a skip call the consumer visits each site
// once, the name site seven more times and the call and varargs sites the producer never called once more, with a
// snapshot after each visit: each folded site folds at its first caching visit, the unreached site caches at its
// first, the name site gives up again at its eighth case, the linked call site and the one called once, which come
// back on the executed-once call data, and the tail call, which comes back with call data of its own and seen, link at
// their first call, the linked one creating its callee's CodeBlock then, the never-called call and varargs sites take
// the two native trips to their link, the construct site stays virtual and the site that gave up with no case never
// caches.
load("./resources/ics.js", "caller relative");

(function main(role) {
    function folds(skip, op, o, v) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            return o.loaded;
        case 2:
            o.stored = v;
            return;
        case 3:
            return "held" in o;
        case 4:
            return o instanceof v;
        }
    }
    const foldsOp = { anchor: 0, loaded: 1, stored: 2, held: 3, instanceOf: 4 };
    // instanceof also loads Symbol.hasInstance and prototype through two get_by_id ICs ahead of its own.
    const foldsIC = { anchor: 0, loaded: 1, stored: 2, held: 3, instanceOf: 6 };
    const foldsLayout = ["GetById", "GetById", "PutByIdSloppy", "InById", "GetById", "GetById", "InstanceOf"];

    // Case n reads one name, so the IC of case n is IC n.
    function limits(skip, op, o) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            return o.dictionary;
        case 2:
            return o.name;
        case 3:
            return o.unreached;
        }
    }
    const limitsSite = { anchor: 0, dictionary: 1, name: 2, unreached: 3 };
    const limitsLayout = ["GetById", "GetById", "GetById", "GetById"];

    // Three op_call sites, a varargs site and a construct site. Case n reads op_call site n - 1.
    function linker(skip, op, o, f, args) {
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
            return f(...args);
        case 5:
            return new f();
        }
    }
    const linkerOp = { anchor: 0, linked: 1, once: 2, never: 3, varargs: 4, construct: 5 };
    const linkerCallSite = { linked: 0, once: 1, never: 2 };

    // In strict code the call in return position is a tail call.
    function tailLinker(skip, op, o, f) {
        "use strict";
        if (skip)
            return;
        if (op === 0)
            return o.anchor;
        return f();
    }

    function target() {
        return 1;
    }
    function Built() {
    }
    function Other() {
    }
    function Instance() {
    }

    function produce() {
        underTest(folds, limits, linker, tailLinker);
        for (const body of [folds, limits, linker, tailLinker])
            toBaseline(body);

        const anchor = { anchor: 1 };
        const alive = [anchor, target, Built, Other, Instance];
        const icOf = (body, index) => snapshot(body).propertyICs[index];
        for (const body of [folds, limits, linker, tailLinker]) {
            body(false, 0, anchor);
            body(false, 0, anchor);
        }

        const foldDrives = [
            ["loaded", o => folds(false, foldsOp.loaded, o)],
            ["stored", o => folds(false, foldsOp.stored, o, 2)],
            ["held", o => folds(false, foldsOp.held, o)],
            ["instanceOf", o => folds(false, foldsOp.instanceOf, o, Instance)],
        ];
        for (const [site, visit] of foldDrives) {
            const objects = shapes(site, 8);
            alive.push(objects);
            drive(`the ${site} fold`, 24, i => visit(objects[i % 8]), () => icOf(folds, foldsIC[site]).megamorphicCaseListed);
        }

        const dictionary = flattenedDictionary("dictionary");
        alive.push(dictionary);
        drive("the give-up with no case", 4, () => limits(false, limitsSite.dictionary, dictionary), () => icOf(limits, limitsSite.dictionary).holdsGaveUp);
        const names = shapes("name", 8);
        alive.push(names);
        drive("the eighth case", 24, i => limits(false, limitsSite.name, names[i % 8]), () => {
            const ic = icOf(limits, limitsSite.name);
            return ic.caseCount === 8 && ic.holdsGaveUp;
        });

        // The never-called call and varargs sites stay as linking left them.
        linker(false, linkerOp.linked, anchor, target);
        linker(false, linkerOp.linked, anchor, target);
        linker(false, linkerOp.once, anchor, target);
        linker(false, linkerOp.construct, anchor, Built);
        linker(false, linkerOp.construct, anchor, Built);
        linker(false, linkerOp.construct, anchor, Other);
        tailLinker(false, 1, anchor, target);

        const foldsState = snapshot(folds);
        expectLayout("folds", foldsState, foldsLayout);
        for (const site of ["loaded", "stored", "held", "instanceOf"])
            expectFields(`folds ${site}`, foldsState.propertyICs[foldsIC[site]], { caseCount: 1, megamorphicCaseListed: true, holdsGaveUp: site === "instanceOf" });
        const limitsState = snapshot(limits);
        expectLayout("limits", limitsState, limitsLayout);
        expectFields("limits dictionary", limitsState.propertyICs[limitsSite.dictionary], { caseCount: 0, holdsGaveUp: true });
        expectFields("limits name", limitsState.propertyICs[limitsSite.name], { caseCount: 8, holdsGaveUp: true, megamorphicCaseListed: false });
        expectFields("limits unreached", limitsState.propertyICs[limitsSite.unreached], { everConsidered: false });
        const linkerState = snapshot(linker);
        expectLayout("linker", linkerState, ["GetById"]);
        const linkerCall = site => callSite("linker", linkerState, "op_call", linkerCallSite[site]);
        expectFields("linker linked", linkerCall("linked"), { data: "Own", mode: "Monomorphic" });
        expectFields("linker once", linkerCall("once"), { data: "ExecutedOnce", mode: "Init", seenOnce: true });
        expectFields("linker never", linkerCall("never"), { data: "NotExecuted", mode: "Init", seenOnce: false });
        expectFields("linker varargs", callSite("linker", linkerState, "op_call_varargs", 0), { data: "Inline", mode: "Init", seenOnce: false });
        expectFields("linker construct", callSite("linker", linkerState, "op_construct", 0), { mode: "Virtual" });
        const tailLinkerState = snapshot(tailLinker);
        expectLayout("tailLinker", tailLinkerState, ["GetById"]);
        expectFields("tailLinker tail call", callSite("tailLinker", tailLinkerState, "op_tail_call", 0), { data: "Own", mode: "Init", seenOnce: true });

        delta(folds, limits, linker, tailLinker);
        keepAlive(alive);
    }

    // The consumer's visits, which an Off run makes too, with their results checked and without the checks of state.
    function consume(checking) {
        underTest(folds, limits, linker, tailLinker);
        for (const body of [folds, limits, linker, tailLinker])
            body(true);

        const anchor = { anchor: 1 };
        const expectIC = (body, label, index, fields) => {
            if (checking)
                expectFields(`${label} after its first consumer visit`, snapshot(body).propertyICs[index], fields);
        };
        const expectCallSite = (label, body, opcode, metadataID, fields) => {
            if (checking)
                expectFields(label, callSite(body.name, snapshot(body), opcode, metadataID), fields);
        };
        if (checking) {
            for (const body of [folds, limits, linker, tailLinker])
                checkSame(snapshot(body).jitType, "Baseline", `${body.name}'s JIT type after its skip call`);
        }

        // Every object a visit caches stays reachable until the checks end, so no collection resets a case first.
        const visited = { loaded: { loaded: 1 }, stored: { stored: 1 }, held: { held: 1 }, instance: { instance: 1 }, dictionary: { dictionary: 1 }, names: shapes("name", 8), unreached: { unreached: 1 } };
        checkSame(folds(false, foldsOp.loaded, visited.loaded), 1, "the folded get's result");
        expectIC(folds, "the folded get", foldsIC.loaded, { caseCount: 1, megamorphicCaseListed: true });
        folds(false, foldsOp.stored, visited.stored, 2);
        checkSame(visited.stored.stored, 2, "the property the folded put stored");
        expectIC(folds, "the folded put", foldsIC.stored, { caseCount: 1, megamorphicCaseListed: true });
        checkSame(folds(false, foldsOp.held, visited.held), true, "the folded in's result");
        expectIC(folds, "the folded in", foldsIC.held, { caseCount: 1, megamorphicCaseListed: true });
        checkSame(folds(false, foldsOp.instanceOf, visited.instance, Instance), false, "the folded instanceof's result");
        expectIC(folds, "the folded instanceof", foldsIC.instanceOf, { caseCount: 1, megamorphicCaseListed: true, holdsGaveUp: true });

        checkSame(limits(false, limitsSite.dictionary, visited.dictionary), 1, "the given-up site's result");
        expectIC(limits, "the site given up with no case", limitsSite.dictionary, { caseCount: 0, holdsGaveUp: true });
        checkSame(limits(false, limitsSite.name, visited.names[0]), 1, "the name site's first result");
        expectIC(limits, "the name site", limitsSite.name, { caseCount: 1, holdsGaveUp: false });
        for (let i = 1; i < 8; ++i)
            checkSame(limits(false, limitsSite.name, visited.names[i]), 1, `the name site's result for shape ${i}`);
        expectIC(limits, "the name site, eight shapes on,", limitsSite.name, { caseCount: 8, holdsGaveUp: true });
        checkSame(limits(false, limitsSite.unreached, visited.unreached), 1, "the unreached site's result");
        expectIC(limits, "the unreached site", limitsSite.unreached, { caseCount: 1 });

        // Each call site as the producer's last execution left it: the linked site and the one called once on the
        // executed-once call data, ready to link at their next call, the tail call with call data of its own and seen,
        // and the sites never called as linking left them.
        expectCallSite("the linked call site before any call", linker, "op_call", linkerCallSite.linked, { data: "ExecutedOnce", mode: "Init", seenOnce: true });
        expectCallSite("the call site called once before any call", linker, "op_call", linkerCallSite.once, { data: "ExecutedOnce", mode: "Init", seenOnce: true });
        expectCallSite("the call site never called before any call", linker, "op_call", linkerCallSite.never, { data: "NotExecuted", mode: "Init", seenOnce: false });
        expectCallSite("the varargs site before any call", linker, "op_call_varargs", 0, { data: "Inline", mode: "Init", seenOnce: false });
        expectCallSite("the tail call before any call", tailLinker, "op_tail_call", 0, { data: "Own", mode: "Init", seenOnce: true });

        // The import creates no callee CodeBlock; the first call links natively and creates it.
        if (checking)
            checkSame(snapshot(target), undefined, "target's CodeBlock before the consumer's first call");
        checkSame(linker(false, linkerOp.linked, anchor, target), 1, "the linked call site's result");
        expectCallSite("the linked call site after one consumer call", linker, "op_call", linkerCallSite.linked, { data: "Own", mode: "Monomorphic" });
        if (checking)
            check(snapshot(target), "target has no CodeBlock after the consumer's first call");
        checkSame(linker(false, linkerOp.once, anchor, target), 1, "the result of the call site called once");
        expectCallSite("the call site called once, after one consumer call", linker, "op_call", linkerCallSite.once, { data: "Own", mode: "Monomorphic" });
        checkSame(tailLinker(false, 1, anchor, target), 1, "the tail call's result");
        expectCallSite("the tail call after one consumer call", tailLinker, "op_tail_call", 0, { data: "Own", mode: "Monomorphic" });

        // The sites the producer never called take the two native trips: the first leaves an op_call site on the
        // executed-once data and an inline site seen, the second links.
        checkSame(linker(false, linkerOp.never, anchor, target), 1, "the never-called call site's first result");
        expectCallSite("the never-called call site after one consumer call", linker, "op_call", linkerCallSite.never, { data: "ExecutedOnce", mode: "Init", seenOnce: true });
        checkSame(linker(false, linkerOp.never, anchor, target), 1, "the never-called call site's second result");
        expectCallSite("the never-called call site after two consumer calls", linker, "op_call", linkerCallSite.never, { data: "Own", mode: "Monomorphic" });
        const spreadArguments = [1, 2];
        checkSame(linker(false, linkerOp.varargs, anchor, target, spreadArguments), 1, "the varargs site's first result");
        expectCallSite("the varargs site after one consumer call", linker, "op_call_varargs", 0, { data: "Inline", mode: "Init", seenOnce: true });
        checkSame(linker(false, linkerOp.varargs, anchor, target, spreadArguments), 1, "the varargs site's second result");
        expectCallSite("the varargs site after two consumer calls", linker, "op_call_varargs", 0, { data: "Inline", mode: "Monomorphic" });

        check(linker(false, linkerOp.construct, anchor, Built) instanceof Built, "the construct site's result is no Built");
        expectCallSite("the construct site after one consumer construction", linker, "op_construct", 0, { mode: "Virtual" });
        keepAlive(visited, spreadArguments);
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
