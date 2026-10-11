// jitcache-runs: Producer --useConcurrentGC=false; Consumer
// jitcache-requires: twins

// SPEC-ics.md T4: what an imported site does at its first consumer visit. The producer folds a get, a put, an in and an
// instanceof site, gives one site up with no case listed, makes a get_by_id of name, a name the megamorphic fold
// refuses, give up on its eighth case, leaves one site unreached, links a call site and sends a construct site virtual,
// each body holding an anchor (its first property IC, with one case). After a skip call the consumer visits each site
// once, and the name site seven more times: each folded site folds at its first caching visit, the unreached site
// caches at its first, the name site gives up again at its eighth case, the call links natively and creates its
// callee's CodeBlock, the construct site stays virtual and the site that gave up with no case never caches.
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

    function linker(skip, op, o, f) {
        if (skip)
            return;
        switch (op) {
        case 0:
            return o.anchor;
        case 1:
            return f();
        case 2:
            return new f();
        }
    }
    const linkerOp = { anchor: 0, call: 1, construct: 2 };

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
        underTest(folds, limits, linker);
        for (const body of [folds, limits, linker])
            toBaseline(body);

        const anchor = { anchor: 1 };
        const alive = [anchor, target, Built, Other, Instance];
        const icOf = (body, index) => snapshot(body).propertyICs[index];
        for (const body of [folds, limits, linker]) {
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

        linker(false, linkerOp.call, anchor, target);
        linker(false, linkerOp.call, anchor, target);
        linker(false, linkerOp.construct, anchor, Built);
        linker(false, linkerOp.construct, anchor, Built);
        linker(false, linkerOp.construct, anchor, Other);

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
        expectFields("linker call", callSite("linker", linkerState, "op_call", 0), { mode: "Monomorphic" });
        expectFields("linker construct", callSite("linker", linkerState, "op_construct", 0), { mode: "Virtual" });

        delta(folds, limits, linker);
        keepAlive(alive);
    }

    // The consumer's visits, which an Off run makes too, with their results checked and without the checks of state.
    function consume(checking) {
        underTest(folds, limits, linker);
        for (const body of [folds, limits, linker])
            body(true);

        const anchor = { anchor: 1 };
        const expectIC = (body, label, index, fields) => {
            if (checking)
                expectFields(`${label} after its first consumer visit`, snapshot(body).propertyICs[index], fields);
        };
        if (checking) {
            for (const body of [folds, limits, linker])
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

        // The import creates no callee CodeBlock; the first call links natively and creates it.
        if (checking)
            checkSame(snapshot(target), undefined, "target's CodeBlock before the consumer's first call");
        checkSame(linker(false, linkerOp.call, anchor, target), 1, "the call site's result");
        if (checking) {
            expectFields("the call site after one consumer call", callSite("linker", snapshot(linker), "op_call", 0), { mode: "Monomorphic" });
            check(snapshot(target), "target has no CodeBlock after the consumer's first call");
        }
        check(linker(false, linkerOp.construct, anchor, Built) instanceof Built, "the construct site's result is no Built");
        if (checking)
            expectFields("the construct site after one consumer construction", callSite("linker", snapshot(linker), "op_construct", 0), { mode: "Virtual" });
        keepAlive(visited);
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
