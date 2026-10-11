// jitcache-runs: Producer --useConcurrentGC=false; ConsumerProducer; Consumer
// jitcache-requires: twins

// SPEC-ics.md T10: a ConsumerProducer's recapture of imported bodies, and two of the criteria it wins on. Body F has a
// get_by_id and an instanceof site the producer folds, an add the producer runs on int32 only and a witness get_by_id
// the producer never reaches; an argument picks which group a call runs. Body G holds a put_by_id and a get_by_id, and
// the producer only brings it to baseline. The ConsumerProducer runs F's add on doubles, which the add MathIC's
// repatching operation records, and F's witness once with a number base, without reaching F's folded sites; F's
// recapture lists no case but is richer, so it beats the saved body. It then runs G's sites, which add no profile
// category, since G's baseline code writes no profile, so G's recapture wins on the IC count; only G is passed to
// delta(), as F has no anchor there. The Consumer finds the witness's non-cell bit, which only F's recapture carries,
// and the folds the recapture passed on, which fold at their first caching visit, and finds G's visited sites considered.
load("./resources/ics.js", "caller relative");

(function main(role) {
    function F(skip, group, o, C, a, b) {
        if (skip)
            return;
        if (group === 0) {
            const loaded = o.folded;
            return o instanceof C ? 0 : loaded;
        }
        if (group === 1)
            return a + b;
        return o.witness;
    }
    // instanceof also loads Symbol.hasInstance and prototype through two get_by_id ICs.
    const fLayout = ["GetById", "GetById", "GetById", "InstanceOf", "GetById"];
    const fSite = { folded: 0, instanceOf: 3, witness: 4 };

    function G(skip, o, v) {
        if (skip)
            return;
        o.stored = v;
        return o.loaded;
    }
    const gLayout = ["PutByIdSloppy", "GetById"];

    function Folded() {
    }

    function produce() {
        underTest(F, G);
        toBaseline(F);
        toBaseline(G);

        const folded = shapes("folded", 8);
        drive("F's folds", 24, i => F(false, 0, folded[i % 8], Folded), () => {
            const ics = snapshot(F).propertyICs;
            return ics[fSite.folded].megamorphicCaseListed && ics[fSite.instanceOf].megamorphicCaseListed;
        });
        for (let i = 0; i < 3; ++i)
            F(false, 1, undefined, undefined, i, i + 1);

        const fState = snapshot(F);
        expectLayout("F", fState, fLayout);
        expectFields("F's folded get", fState.propertyICs[fSite.folded], { caseCount: 1, megamorphicCaseListed: true });
        expectFields("F's folded instanceof", fState.propertyICs[fSite.instanceOf], { caseCount: 1, megamorphicCaseListed: true, holdsGaveUp: true });
        expectFields("F's witness", fState.propertyICs[fSite.witness], { everConsidered: false, sawNonCell: false });
        const gState = snapshot(G);
        expectLayout("G", gState, gLayout);
        gState.propertyICs.forEach((ic, index) => expectFields(`G's IC ${index}`, ic, { everConsidered: false, caseCount: 0 }));

        delta(F);
        keepAlive(folded, Folded);
    }

    function consumeAndProduce() {
        underTest(F, G);
        F(true);
        G(true);
        for (const [label, state] of [["F", snapshot(F)], ["G", snapshot(G)]]) {
            check(state, `${label} has no CodeBlock after its skip call`);
            checkSame(state.jitType, "Baseline", `${label}'s JIT type after its skip call`);
        }
        expectFields("F's imported folded get", snapshot(F).propertyICs[fSite.folded], { canBeMegamorphic: true, caseCount: 0 });

        for (let i = 0; i < 3; ++i)
            F(false, 1, undefined, undefined, i + 0.5, i + 0.25);
        F(false, 2, 7);
        const stored = { stored: 0, loaded: 1 };
        G(false, stored, 1);
        G(false, stored, 2);

        delta(G);
        keepAlive(stored);
    }

    // The Consumer's path, which an Off run follows too, with its result checked and without the checks of state.
    function consume(checking) {
        underTest(F, G);
        F(true);
        G(true);
        if (checking) {
            const fICs = snapshot(F).propertyICs;
            expectFields("F's witness before any visit", fICs[fSite.witness], { sawNonCell: true });
            expectFields("F's folded get before any visit", fICs[fSite.folded], { canBeMegamorphic: true, caseCount: 0 });
            expectFields("F's folded instanceof before any visit", fICs[fSite.instanceOf], { canBeMegamorphic: true, caseCount: 0, holdsGaveUp: false });
            snapshot(G).propertyICs.forEach((ic, index) => expectFields(`G's IC ${index} before any visit`, ic, { everConsidered: true, caseCount: 0 }));
        }

        checkSame(F(false, 0, { folded: 1 }, Folded), 1, "F's result through its folded sites");
        if (checking) {
            const fICs = snapshot(F).propertyICs;
            expectFields("F's folded get after one caching visit", fICs[fSite.folded], { caseCount: 1, megamorphicCaseListed: true });
            expectFields("F's folded instanceof after one caching visit", fICs[fSite.instanceOf], { caseCount: 1, megamorphicCaseListed: true, holdsGaveUp: true });
        }
    }

    switch (role) {
    case "Producer":
        produce();
        break;
    case "ConsumerProducer":
        consumeAndProduce();
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
