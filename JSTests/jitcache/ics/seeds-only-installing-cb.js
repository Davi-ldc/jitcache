// jitcache-runs: Producer --useConcurrentGC=false; Consumer
// jitcache-requires: twins

// SPEC-ics.md T8: the seeds land only in the CodeBlock that installs the import. The bodies live in a source string that
// runString evaluates in a new realm each time; the same text hits the same CodeCache entry, so the realms share the
// bodies' UnlinkedCodeBlocks. The producer runs the string once, drives the bodies' sites, each body holding an anchor
// (its first property IC, with one case), saves their snapshots and passes them to delta(). The consumer runs the string
// twice. The first realm's CodeBlock of each body installs the import and restores what section 6.1 derives from the
// saved snapshot; the second realm's, created next, takes the parked image natively, with every IC never considered and
// every call-link site as linking leaves it.
load("./resources/ics.js", "caller relative");

(function main(role) {
    const source = `
function seededGets(skip, op, o) {
    if (skip)
        return;
    switch (op) {
    case 0:
        return o.anchor;
    case 1:
        return o.other;
    }
}

function seededCalls(skip, op, o, f) {
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
`;

    function target() {
        return 1;
    }
    function Built() {
    }
    function Other() {
    }

    // A realm of the source, with its bodies passed to underTest before their first call.
    function realmBodies() {
        const realm = runString(source);
        const bodies = { seededGets: realm.seededGets, seededCalls: realm.seededCalls };
        underTest(...Object.values(bodies));
        return bodies;
    }

    function produce() {
        const bodies = realmBodies();
        for (const body of Object.values(bodies))
            toBaseline(body);

        const anchor = { anchor: 1 };
        const others = shapes("other", 2);
        const { seededGets, seededCalls } = bodies;
        seededGets(false, 0, anchor);
        seededGets(false, 0, anchor);
        for (const o of [others[0], others[0], others[1]])
            seededGets(false, 1, o);
        seededCalls(false, 0, anchor);
        seededCalls(false, 0, anchor);
        seededCalls(false, 1, anchor, target);
        seededCalls(false, 1, anchor, target);
        seededCalls(false, 2, anchor, Built);
        seededCalls(false, 2, anchor, Built);
        seededCalls(false, 2, anchor, Other);

        const saved = {};
        for (const [name, body] of Object.entries(bodies))
            saved[name] = snapshot(body);
        expectLayout("seededGets", saved.seededGets, ["GetById", "GetById"]);
        expectFields("seededGets anchor", saved.seededGets.propertyICs[0], { caseCount: 1 });
        expectFields("seededGets other", saved.seededGets.propertyICs[1], { caseCount: 2 });
        expectLayout("seededCalls", saved.seededCalls, ["GetById"]);
        expectFields("seededCalls anchor", saved.seededCalls.propertyICs[0], { caseCount: 1 });
        expectFields("seededCalls call", callSite("seededCalls", saved.seededCalls, "op_call", 0), { mode: "Monomorphic", seenOnce: true });
        expectFields("seededCalls construct", callSite("seededCalls", saved.seededCalls, "op_construct", 0), { mode: "Virtual" });

        saveJSON("seeds-only-installing-cb", saved);
        delta(...Object.values(bodies));
        keepAlive(anchor, others, target, Built, Other);
    }

    function consume(checking) {
        const installing = realmBodies();
        for (const body of Object.values(installing))
            body(true);
        if (checking) {
            const saved = loadJSON("seeds-only-installing-cb");
            for (const [name, body] of Object.entries(installing))
                expectRestored(`${name} of the first realm`, snapshot(body), saved[name]);
        }

        const sharing = realmBodies();
        for (const body of Object.values(sharing))
            body(true);
        if (checking) {
            for (const [name, body] of Object.entries(sharing))
                expectFresh(`${name} of the second realm`, snapshot(body));
        }
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
