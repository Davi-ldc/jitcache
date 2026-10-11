// jitcache-runs: Producer --useConcurrentGC=false; Consumer
// jitcache-requires: twins

// SPEC-ics.md T5: what an optimizing compile would read from an imported IC before its first consumer visit, as
// PropertyInlineCache::summary gives it. The producer gives a put_by_id site cases (the body's anchor), gives a second
// site up with no case listed, sends a non-cell base through a third, leaves a fourth unreached, and makes a fifth, a
// get_by_id of name, give up on its eighth case and then visits it only with those eight shapes, so its tookSlowPath
// stays clear. Before any visit the consumer reads the put site as considered and empty (Simple, the generic put of
// THREAD Caches), the given-up site and the non-cell site as slow, the unreached site as never considered and the fifth
// site as considered and empty, back on its *Optimize operation.
load("./resources/ics.js", "caller relative");

(function main(role) {
    function statuses(skip, op, o, v) {
        if (skip)
            return;
        switch (op) {
        case 0:
            o.anchor = v;
            return;
        case 1:
            return o.dictionary;
        case 2:
            return o.nonCell;
        case 3:
            return o.unreached;
        case 4:
            return o.name;
        }
    }
    const site = { anchor: 0, dictionary: 1, nonCell: 2, unreached: 3, name: 4 };

    function produce() {
        underTest(statuses);
        toBaseline(statuses);

        const icOf = index => snapshot(statuses).propertyICs[index];
        const anchor = { anchor: 0 };
        statuses(false, site.anchor, anchor, 1);
        statuses(false, site.anchor, anchor, 2);
        const dictionary = flattenedDictionary("dictionary");
        drive("the give-up with no case", 4, () => statuses(false, site.dictionary, dictionary), () => icOf(site.dictionary).holdsGaveUp);
        statuses(false, site.nonCell, 0);
        const names = shapes("name", 8);
        drive("the eighth case", 24, i => statuses(false, site.name, names[i % 8]), () => {
            const ic = icOf(site.name);
            return ic.caseCount === 8 && ic.holdsGaveUp;
        });
        // Each shape hits one of the eight cases, so no visit reaches the *GaveUp operation.
        for (const o of names)
            statuses(false, site.name, o);

        const state = snapshot(statuses);
        expectLayout("statuses", state, ["PutByIdSloppy", "GetById", "GetById", "GetById", "GetById"]);
        expectFields("the put site", state.propertyICs[site.anchor], { caseCount: 1 });
        expectFields("the given-up site", state.propertyICs[site.dictionary], { caseCount: 0, holdsGaveUp: true });
        expectFields("the non-cell site", state.propertyICs[site.nonCell], { sawNonCell: true });
        expectFields("the unreached site", state.propertyICs[site.unreached], { everConsidered: false });
        expectFields("the name site", state.propertyICs[site.name], { caseCount: 8, holdsGaveUp: true, tookSlowPath: false });

        delta(statuses);
        keepAlive(anchor, dictionary, names);
    }

    function consume(checking) {
        underTest(statuses);
        statuses(true);
        if (!checking)
            return;
        const ics = snapshot(statuses).propertyICs;
        expectFields("the put site", ics[site.anchor], { summary: "Simple", everConsidered: true, caseCount: 0 });
        expectFields("the given-up site", ics[site.dictionary], { summary: "TakesSlowPath", tookSlowPath: true, holdsGaveUp: true });
        expectFields("the non-cell site", ics[site.nonCell], { summary: "TakesSlowPath", sawNonCell: true });
        expectFields("the unreached site", ics[site.unreached], { summary: "NoInformation", everConsidered: false });
        expectFields("the name site", ics[site.name], { summary: "Simple", holdsGaveUp: false, tookSlowPath: false, caseCount: 0 });
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
