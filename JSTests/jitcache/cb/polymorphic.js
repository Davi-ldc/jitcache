// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --verboseOSR=true
// jitcache-check: 1 resources/polymorphic-check.ts

// SPEC-cb.md section 11.3, a polymorphic body (section 5.3, I16). polyBody reads one property from objects of three
// structures, so its get_by_id IC lists three cases and the ICs lane reports a polymorphic site. Its Producer runs it to
// the last call before its counter would cross, as floor.js does with a probe of the same text, and delta captures it
// there. The capture carries no counter: cb.state and cb.summary say NotCarried, with zero counter fields and a zero
// counterProgress, which a twins Producer checks. In the Consumer the counter therefore starts from setup's arming, so
// polyBody enters operationOptimize only after the native warm-up, where a carried counter at its threshold would have
// crossed on the second invocation; resources/polymorphic-check.ts reads the lines operationOptimize writes on entry under
// verboseOSR, against a marker the script writes on stderr before each invocation. The Producer, whose polyBody must stay
// short of crossing, makes the same calls on the probe, which returns the same values.
(function main(role) {
    function polyBody(o) {
        return o.x + 1;
    }
    function polyProbe(o) {
        return o.x + 1;
    }
    noInline(polyBody);
    noInline(polyProbe);
    const shapes = [{ x: 1 }, { y: 0, x: 2 }, { z: 0, x: 3 }];
    const input = i => shapes[i % 3];

    if (role === "Producer") {
        let calls = 0;
        while (!numberOfDFGCompiles(polyProbe) && calls < 100000)
            polyProbe(input(calls++));
        if (!numberOfDFGCompiles(polyProbe))
            throw new Error("polyProbe never reached the DFG");
        for (let i = 0; i < calls - 1; ++i)
            polyBody(input(i));
        if (numberOfDFGCompiles(polyBody))
            throw new Error("polyBody reached the DFG in the Producer");

        if (typeof jitcacheDelta === "function") {
            jitcacheDelta();
            const sections = load("./resources/cb-sections.js", "caller relative");
            const { state, summary } = sections.checkBody(polyBody, "call", "polyBody");
            const header = state.header;
            if (header.counterMode !== sections.CounterMode.NotCarried || header.counterValue || header.counterTotalCount || header.counterActiveThreshold)
                throw new Error(`polyBody's cb.state carries its counter: ${sections.describe(header)}`);
            if (summary.header.counterMode !== sections.CounterMode.NotCarried || summary.header.counterProgress)
                throw new Error(`polyBody's cb.summary carries counter progress: ${sections.describe(summary.header)}`);
        }
    }

    const call = role === "Producer" ? polyProbe : polyBody;
    let total = 0;
    for (let invocation = 1; invocation <= 200; ++invocation) {
        printErr(`cb-polymorphic invocation ${invocation}`);
        total += call(input(invocation));
    }
    print(total);
})(...arguments);
