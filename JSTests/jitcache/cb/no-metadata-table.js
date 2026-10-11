// jitcache-runs: Producer --useConcurrentJIT=false; Consumer --useConcurrentJIT=false
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-cb.md section 11.3: hot bodies without a metadata table (N1, I15), one with a property IC and one without, are
// captured at both capture points and installed with strict on. In the first sequence the Producer commits only the
// capture its baseline compilation makes; in the second, delta's capture replaces it. Each body must still run baseline
// code when delta runs, so the Producer first calls a probe with the same text until the probe's DFG compilation and then
// calls the body half as often: past its baseline compilation and well short of its DFG one. noInline keeps every
// compilation to its own body. In twins builds the Producer checks the committed sections: no value profiles, no family
// entries, both argument predictions, and the counter progress of the capture point that committed them; the Consumer
// checks that both bodies were imported at their first call.
(function main(role, scratch, artifact, sequence) {
    function id(x) {
        return x;
    }
    function has(o) {
        return "x" in o;
    }
    function idProbe(x) {
        return x;
    }
    function hasProbe(o) {
        return "x" in o;
    }
    for (const fn of [id, has, idProbe, hasProbe])
        noInline(fn);

    const idArgument = i => (i & 1 ? `s${i & 3}` : i);
    const hasArgument = i => (i & 1 ? { x: i } : { x: 0 });
    function callsUntilDFG(probe, argument) {
        let calls = 0;
        while (!numberOfDFGCompiles(probe) && calls < 100000)
            probe(argument(calls++));
        return calls;
    }

    if (role === "Producer") {
        for (const [body, probe, argument] of [[id, idProbe, idArgument], [has, hasProbe, hasArgument]]) {
            const calls = callsUntilDFG(probe, argument) >> 1;
            for (let i = 0; i < calls; ++i)
                body(argument(i));
        }
    }

    // One structure reaches `has`, so its in_by_id IC holds one case and its counter travels (section 5.3).
    let total = 0;
    for (let i = 0; i < 8; ++i)
        total += `${id(idArgument(i))}`.length + (has(hasArgument(i)) ? i : 0);
    print(total);

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(id, "call", "id");
        sections.expectImported(has, "call", "has");
        return;
    }
    if (role !== "Producer" || typeof jitcacheDelta !== "function")
        return;

    // The second sequence's delta, so the check reads its capture; the first sequence reads the baseline compilation's.
    const fromDelta = sequence === "1";
    if (fromDelta) {
        fullGC();
        jitcacheDelta();
    }
    const sections = load("./resources/cb-sections.js", "caller relative");
    for (const [fn, label] of [[id, "id"], [has, "has"]]) {
        const { state, summary } = sections.checkBody(fn, "call", label);
        const header = state.header;
        if (header.numValueProfiles || header.familyEntryCount.some(count => count))
            throw new Error(`${label}: a CB without a metadata table committed ${header.numValueProfiles} value profiles and family counts ${header.familyEntryCount}`);
        if (header.numArguments !== 2 || !state.arguments[1])
            throw new Error(`${label}: cb.state holds ${header.numArguments} arguments, and argument 1 predicts ${sections.describe(state.arguments[1])}`);
        // The baseline compilation's capture sees the counter setup just armed, less than one point of progress; delta's
        // sees the progress of the calls since.
        const progress = summary.header.counterProgress;
        if (fromDelta ? !progress : progress)
            throw new Error(`${label}: the committed capture has counterProgress ${progress}, which is not ${fromDelta ? "delta's" : "the baseline compilation's"}`);
    }
})(...arguments);
