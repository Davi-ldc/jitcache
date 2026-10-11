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
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=13; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=14; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=15; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=16; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=17; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=18; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=19; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=20; Consumer
// jitcache-require-fault: 0 exec-alloc.baseline-plan
// jitcache-require-fault: 0 exec-alloc.dfg-plan
// jitcache-require-fault: 0 exec-alloc.ftl-plan
// jitcache-require-fault: 0 exec-alloc.ic-handler
// jitcache-require-fault: 0 exec-alloc.mathic-snippet
// jitcache-expect-no-install: 1

// SPEC-integrator.md section 15.2, exec-alloc-faults.js. The fuzzer fails the n-th executable allocation that may fail
// (ExecutableAllocator::allocate counts every JITCompilationCanFail request), and the directory's default keeps
// concurrent JIT off, so each n names one allocation of the script's fixed order, and everything before it runs as with
// no fuzzing. The script reaches all five fault sites early, nothing allocating such memory before its first plan:
// - eaReads's baseline plan at its 34th call; each of its two sites reads a global-object property through the global
//   proxy, which gets no shared handler, so the second visit compiles one stub per site, in site order (SPEC-ics.md T9);
// - eaAdd's baseline plan, compiled from integer operands, and its add MathIC's out-of-line snippet at the first double;
// - eaSpin's baseline plan at its loop's OSR entry, its DFG plan and its FTL plans, all within one call whose loop takes
//   no speculation exit.
// For each n the Producer's fault, when there is one, is one of the five steps, and jitcacheStatus() names it; a body
// whose baseline plan failed never compiles again, so its fault is the baseline plan's. eaAdd stays in baseline code
// through the delta at exit, so the image twin, which replays its MathIC regeneration, catches a snippet fault a capture
// held (SPEC-image.md section 11.3). The Consumer runs unfuzzed, calls every function, and reads each committed body's
// sections: no ucb.feedback holds a deferred LLInt counter, an m_activeThreshold of INT32_MAX (SPEC-ucb.md section 5.2),
// and every cb.state carries its baseline counter (no site is polymorphic, SPEC-cb.md I16), never a deferred one
// (SPEC-cb.md section 3.2), which a plan fault raised after the plan's effects would leave. No body reoptimizes, where
// adjustedCounterValue would defer a counter natively. A Producer whose fault came early commits little or nothing, so
// its Consumer may install nothing. The runner requires each step from some n and compares every output with the
// JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const faultSteps = ["exec-alloc.baseline-plan", "exec-alloc.dfg-plan", "exec-alloc.ftl-plan", "exec-alloc.ic-handler", "exec-alloc.mathic-snippet"];

    function eaReads(skip, g) {
        if (skip)
            return null;
        return [g.eaViaProxy0, g.eaViaProxy1];
    }

    function eaAdd(a, b) {
        return a + b;
    }

    function eaSpin(count) {
        let sum = 0;
        for (let i = 0; i < count; ++i)
            sum = (sum + (i & 1023)) & 0xfffffff;
        return sum;
    }

    globalThis.eaViaProxy0 = 1;
    globalThis.eaViaProxy1 = 2;

    for (let i = 0; i < 40; ++i)
        eaReads(true, globalThis);
    let reads = 0;
    for (let visit = 0; visit < 3; ++visit) {
        const values = eaReads(false, globalThis);
        reads += values[0] + values[1];
    }

    let sums = 0;
    for (let i = 0; i < 40; ++i)
        sums += eaAdd(i, 1);
    sums += eaAdd(0.5, 1) + eaAdd(1.5, 2);

    const spun = eaSpin(1000000);
    print(`eaReads read ${reads}, eaAdd added ${sums}, eaSpin spun ${spun}`);

    if (role === "Producer") {
        const fault = jitcacheStatus();
        check(fault === null || faultSteps.includes(fault), `the Producer's fault is ${fault}, none of the five executable-allocation steps`);
        for (const fn of [eaReads, eaAdd, eaSpin]) {
            if (!events(fn).baselineCompiles)
                checkSame(fault, "exec-alloc.baseline-plan", `the fault of a run in which ${fn.name} never compiled`);
        }
        checkSame(events(eaAdd).dfgCompiles, 0, "eaAdd's DFG compiles, which would take it out of baseline code before the delta");
        return;
    }

    if (role === "Consumer") {
        for (const fn of [main, eaReads, eaAdd, eaSpin]) {
            const feedback = sectionOf(fn, "call", "ucb.feedback");
            if (feedback === null)
                continue;
            check(i32(feedback, 40) !== INT32_MAX, `${fn.name}'s committed ucb.feedback holds a deferred LLInt counter`);
            const state = sectionOf(fn, "call", "cb.state");
            check(state !== null, `${fn.name}'s committed body has no cb.state`);
            checkSame(u8(state, 3), 1, `the counter mode of ${fn.name}'s cb.state, which has no polymorphic site`);
            check(i32(state, 28) !== INT32_MAX, `${fn.name}'s committed cb.state carries a deferred baseline counter`);
        }
    }
})(...arguments);
