// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-cb.md section 11.3, profile deferrals (P10) and reoptimization counts (P11).
// - lowCoverage keeps most of its value profiles in a branch it never takes, so each time its counter crosses,
//   shouldOptimizeNowFromBaseline finds the profiles too sparse, counts a deferral and re-arms the counter, until
//   maximumOptimizationDelay deferrals let it compile. A probe with the same text, called until it compiles, gives the
//   number of calls that takes; lowCoverage gets half of them, which leaves it in baseline with deferrals counted.
// - stormy compiles to the DFG on integers, then exits on doubles until its DFG code is jettisoned with a counted
//   reoptimization, which makes its baseline CB its executable's replacement again.
// delta captures both after a full collection whose finalization drains the doubles into the predictions, so its
// captures are richer than the ones the baseline compilations made and replace them. In twins builds the Producer checks
// the committed counts. The Consumer checks in every build that stormy's newborn CB carries the reoptimization count,
// which setup then folds into its threshold (adjustedCounterValue).
(function main(role) {
    function lowCoverage(x, rare) {
        if (rare)
            return rare.a.b + rare.c.d + rare.e(rare.f) + rare.g[rare.h] + rare.i.j.k + rare.l.m;
        return x + 1;
    }
    function lowCoverageProbe(x, rare) {
        if (rare)
            return rare.a.b + rare.c.d + rare.e(rare.f) + rare.g[rare.h] + rare.i.j.k + rare.l.m;
        return x + 1;
    }
    function stormy(x) {
        return x * 3 + 1;
    }
    for (const fn of [lowCoverage, lowCoverageProbe, stormy])
        noInline(fn);

    if (role === "Producer") {
        let calls = 0;
        while (!numberOfDFGCompiles(lowCoverageProbe) && calls < 100000)
            lowCoverageProbe(calls++, 0);
        for (let i = 0; i < calls >> 1; ++i)
            lowCoverage(i, 0);
        lowCoverage(0.5, 0);

        for (let i = 0; i < 100000 && !numberOfDFGCompiles(stormy); ++i)
            stormy(i);
        for (let i = 0; i < 100000 && !reoptimizationRetryCount(stormy); ++i)
            stormy(i + 0.5);
        if (!reoptimizationRetryCount(stormy))
            throw new Error("stormy's exits never jettisoned its DFG code");

        fullGC();
        if (typeof jitcacheDelta === "function") {
            jitcacheDelta();
            const sections = load("./resources/cb-sections.js", "caller relative");
            const low = sections.checkBody(lowCoverage, "call", "lowCoverage").state.header;
            if (!low.optimizationDelayCounter)
                throw new Error("lowCoverage: the committed capture counts no profile deferral");
            if (numberOfDFGCompiles(lowCoverage))
                throw new Error("lowCoverage reached the DFG before delta");
            const storm = sections.checkBody(stormy, "call", "stormy").state.header;
            if (!storm.reoptimizationRetryCounter)
                throw new Error("stormy: the committed capture counts no reoptimization");
        }
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += lowCoverage(i, 0) + stormy(i & 1 ? i : i + 0.25);
    print(total);

    if (role === "Consumer" && reoptimizationRetryCount(stormy) < 1)
        throw new Error("stormy's CB carries no reoptimization count");
    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(lowCoverage, "call", "lowCoverage");
        sections.expectImported(stormy, "call", "stormy");
    }
})(...arguments);
