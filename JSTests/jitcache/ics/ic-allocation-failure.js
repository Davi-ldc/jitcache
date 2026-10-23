// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=1; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=2; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=3; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=4; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=5; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=6; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=7; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=8; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=9; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=10; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=11; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=12; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=13; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=14; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=15; Consumer
// jitcache-runs: Producer --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=16; Consumer
// jitcache-requires: twins
// jitcache-pin: off its Off runs call jitcacheICsSnapshot, which only twins builds have
// jitcache-expect-fault: 0 exec-alloc.baseline-plan
// jitcache-expect-fault: 0 exec-alloc.dfg-plan
// jitcache-expect-fault: 0 exec-alloc.ftl-plan
// jitcache-expect-fault: 0 exec-alloc.mathic-snippet
// jitcache-require-fault: 0 exec-alloc.ic-handler
// jitcache-expect-no-install: 0:1

// An IC stub or handler that cannot get executable memory raises exec-alloc.ic-handler before the slow path writes its
// give-up. The fuzzer fails the n-th executable allocation that may fail:
// ExecutableAllocator::allocate counts every JITCompilationCanFail request of every thread, and the directory's default
// keeps concurrent JIT off, so each n names one allocation. The first two are the baseline plans of reads and of ics.js's
// snapshot, which reachBaseline calls the same number of times. The next thirty-two are the stubs of reads's sites,
// compiled in site order at its second visit, so this range fails both plans and fourteen stubs. Every run checks,
// natively, that each visit reads the right values and that at most one site gave up, listing no case. A Producer also
// checks that its status names what failed: exec-alloc.baseline-plan when reads stayed in the LLInt,
// exec-alloc.ic-handler exactly when a site gave up. Whatever the fault, delta() then throws naming it. Each Consumer
// imports what its Producer committed. The fault ended production before anything could save what the visits taught, so
// the Producer committed reads, if at all, at its compilation, before any visit: a Consumer's reads, once in baseline and
// before any of its sites runs, has every IC as installation builds it, holding no give-up and no tookSlowPath. Only the
// first sequence's Producer, whose first allocation failed, commits nothing; from the second on, the plan that finalized
// first committed its body, which the Consumer installs. The runner checks that some run reports exec-alloc.ic-handler,
// that every Consumer but the first installs a body and that each output equals the JITCache-off run's.
load("./resources/ics.js", "caller relative");

(function main(role) {
    check(role === "Producer" || role === "Consumer" || role === "Off", `unknown role ${role}`);
    const siteCount = 32;

    // Each site reads its own property of the global object through the global proxy. Such a load gets neither a handler
    // thunk nor a stateless stub (InlineCacheCompiler::compileOneAccessCaseHandler), and the sites' cases differ in
    // offset, so none reuses another's stub (AccessCase::canBeShared): each site's first caching visit compiles a stub
    // of its own.
    function reads(skip, g) {
        if (skip)
            return;
        return [
            g.viaProxy0, g.viaProxy1, g.viaProxy2, g.viaProxy3, g.viaProxy4, g.viaProxy5, g.viaProxy6, g.viaProxy7,
            g.viaProxy8, g.viaProxy9, g.viaProxy10, g.viaProxy11, g.viaProxy12, g.viaProxy13, g.viaProxy14, g.viaProxy15,
            g.viaProxy16, g.viaProxy17, g.viaProxy18, g.viaProxy19, g.viaProxy20, g.viaProxy21, g.viaProxy22, g.viaProxy23,
            g.viaProxy24, g.viaProxy25, g.viaProxy26, g.viaProxy27, g.viaProxy28, g.viaProxy29, g.viaProxy30, g.viaProxy31,
        ];
    }

    // toBaseline, except that it returns whether fn got there instead of throwing. The fuzz point can be fn's own
    // baseline plan, whose failure defers fn's LLInt counter for good (dontJITAnytimeSoon); 1,000 calls are many times
    // the 34 the default threshold needs.
    function reachBaseline(fn) {
        for (let calls = 0; calls < 1000; ++calls) {
            const state = snapshot(fn);
            if (state && state.jitType === "Baseline")
                return true;
            fn(true);
        }
        return false;
    }

    function checkValues(label, values) {
        checkSame(values.length, siteCount, `the number of values ${label} read`);
        for (let site = 0; site < siteCount; ++site)
            checkSame(values[site], site, `the value site ${site} read at ${label}`);
    }

    // Returns how many sites gave up. Every site the fuzzer spared lists the one case a visit cached: the second, or the
    // first at a site an import brought back with its countdown at zero. The site whose stub the fuzzer failed got
    // GiveUpOnCache, which installed operationGetByIdGaveUp beside no case, and the third visit reached that operation.
    function checkSites() {
        const ics = snapshot(reads).propertyICs;
        checkSame(ics.length, siteCount, "the number of reads's property ICs");
        let givenUp = 0;
        for (let site = 0; site < siteCount; ++site) {
            const label = `reads's site ${site}`;
            checkSame(ics[site].accessType, "GetById", `${label}'s access type`);
            if (ics[site].holdsGaveUp) {
                expectFields(label, ics[site], { caseCount: 0, cacheType: "Stub", tookSlowPath: true });
                ++givenUp;
            } else
                expectFields(label, ics[site], { caseCount: 1, tookSlowPath: false });
        }
        check(givenUp <= 1, `${givenUp} of reads's sites gave up, while the fuzzer fails one allocation`);
        return givenUp;
    }

    for (let site = 0; site < siteCount; ++site)
        globalThis[`viaProxy${site}`] = site;

    underTest(reads);
    const inBaseline = reachBaseline(reads);
    // reachBaseline calls reads with skip, so none of its sites has run yet.
    if (role === "Consumer" && inBaseline)
        expectFresh("reads in a Consumer", snapshot(reads));
    // The first visit spends each IC's countdown and the second caches at every site, in site order
    // (PropertyInlineCache::considerRepatchingCacheImpl). The third runs what the second left.
    const first = reads(false, globalThis);
    const second = reads(false, globalThis);
    const third = reads(false, globalThis);
    checkValues("the first visit", first);
    checkValues("the second visit", second);
    checkValues("the third visit", third);
    const givenUp = inBaseline ? checkSites() : 0;

    if (role === "Producer") {
        const fault = status();
        if (!inBaseline)
            checkSame(fault, "exec-alloc.baseline-plan", "the fault of a run whose reads stayed in the LLInt");
        checkSame(givenUp, fault === "exec-alloc.ic-handler" ? 1 : 0, `the number of sites that gave up under the fault ${fault}`);
        if (fault !== null) {
            let thrown = null;
            try {
                delta();
            } catch (error) {
                thrown = error;
            }
            check(thrown, `delta() returned after the fault ${fault}`);
            checkSame(thrown.message, fault, "the message delta() throws");
        }
    }

    let total = 0;
    for (const values of [first, second, third]) {
        for (const value of values)
            total += value;
    }
    print(`${siteCount} sites read ${total} over three visits`);
})(role());
