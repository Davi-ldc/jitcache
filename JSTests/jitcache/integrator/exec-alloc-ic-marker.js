// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=1; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=2; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=3; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=4; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=5; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=6; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=7; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=8; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=9; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=10; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=11; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=12; Consumer
// jitcache-expect-fault: 0 exec-alloc.baseline-plan
// jitcache-require-fault: 0 exec-alloc.ic-handler
// jitcache-expect-no-install: 1

// SPEC-integrator.md section 15.2, exec-alloc-ic-marker.js: the IC-handler site's marker. Like exec-alloc-faults.js, but
// every IC site of the script can give up only through the fault of its own stub's allocation. emReads's eight sites
// each read a global-object property through the global proxy: one plain, cacheable shape per site, and a first case
// that compiles a stub of its own, since such a load gets neither a handler thunk nor a stateless stub and the sites'
// offsets differ (SPEC-ics.md T9). The first allocation that may fail is emReads's baseline plan at its 34th call, and the
// next eight are the stubs its second visit compiles in site order, so the range fails the plan and every stub. A stub
// that cannot be allocated raises exec-alloc.ic-handler before the slow path writes the give-up (SPEC-ics.md E5), which
// turns activity off, so the body the run committed is the one captured at emReads's compilation, before any visit.
// emReads stays in baseline code through the delta at exit, which commits its warm ICs in the runs whose stubs all
// compiled; main, whose loops stay short of the LLInt's threshold, never compiles and has no body. The Consumer reads
// each function's ICsBaseline section, when its key holds a body (SPEC-ics.md section 4): no record holds tookSlowPath
// or a site that gave up, which would mean the fault came after the give-up was written, or that a stub failed without
// it. The runner requires the step from some n and compares every output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const siteCount = 8;

    function emReads(skip, g) {
        if (skip)
            return null;
        return [g.emViaProxy0, g.emViaProxy1, g.emViaProxy2, g.emViaProxy3, g.emViaProxy4, g.emViaProxy5, g.emViaProxy6, g.emViaProxy7];
    }

    for (let site = 0; site < siteCount; ++site)
        globalThis[`emViaProxy${site}`] = site;

    for (let i = 0; i < 40; ++i)
        emReads(true, globalThis);
    // The checks call nothing that could reach a baseline compilation of its own between the visits.
    let total = 0;
    for (let visit = 0; visit < 3; ++visit) {
        const values = emReads(false, globalThis);
        for (let site = 0; site < siteCount; ++site) {
            if (values[site] !== site)
                fail(`site ${site} read ${values[site]} at visit ${visit}`);
            total += values[site];
        }
    }
    print(`emReads's ${siteCount} sites read ${total} over three visits`);

    if (role === "Producer") {
        const fault = jitcacheStatus();
        check(fault === null || fault === "exec-alloc.baseline-plan" || fault === "exec-alloc.ic-handler", `the Producer's fault is ${fault}`);
        if (!events(emReads).baselineCompiles)
            checkSame(fault, "exec-alloc.baseline-plan", "the fault of a run in which emReads never compiled");
        checkSame(events(emReads).dfgCompiles, 0, "emReads's DFG compiles, which would take it out of baseline code before the delta");
        return;
    }

    if (role === "Consumer") {
        for (const fn of [main, emReads]) {
            const section = sectionOf(fn, "call", "ICsBaseline");
            if (section === null)
                continue;
            // The header's four words, the call-link groups, then one 8-byte record per property IC: learningBits at
            // byte 1, whose bit 2 is tookSlowPath, and stateBits at byte 5, whose bit 2 is holdsGaveUp.
            const propertyICCount = u32(section, 0);
            const records = 16 + 8 * u32(section, 8);
            if (fn === emReads)
                checkSame(propertyICCount, siteCount, "the property-IC records of emReads's ICsBaseline");
            for (let site = 0; site < propertyICCount; ++site) {
                const record = records + 8 * site;
                if (u8(section, record + 1) & 4)
                    fail(`${fn.name}'s committed site ${site} holds tookSlowPath`);
                if (u8(section, record + 5) & 4)
                    fail(`${fn.name}'s committed site ${site} gave up`);
            }
        }
    }
})(...arguments);
