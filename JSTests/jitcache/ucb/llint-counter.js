// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1

// SPEC-ucb.md section 13.3, llint-counter.js, sections 5.5 and I12: the UCB's LLInt counter travels as the producer
// left it and the import arms it anew, with no CodeBlock and no jitSoon after it. One function, llcBody, is the
// completion value of a program text, so it leaves no binding behind.
// - Sequence 0 captures it only right after its baseline compilation (no delta): jitSoon armed the counter with
//   thresholdForJITSoon scaled by the executable pool's memory-pressure multiplier, which exceeds 1 as soon as anything
//   is allocated there, so the progress it leaves is fractional (F20).
// - Sequence 1 also runs a second CodeBlock of the same UCB, created before the compile, three times in the LLInt
//   after it, which adds 45 points to the UCB's counter, and captures again with jitcacheDelta while the compiled
//   CodeBlock lives (delta at exit in a plain build), so the captured progress is about 45 points further.
// In each Consumer T4 checks at the import that the counter holds the captured threshold T and progress P, up to the
// float's rounding (section 13.2). To watch the crossing, the Consumer evaluates the text as tainted code
// ($vm.runTaintedString), whose CodeBlocks' taint differs from the producer's, a baked fact: the install leaves the
// CodeBlock native (SPEC-image.md section 7), so it runs in the LLInt on the counter the import armed, and its native
// baseline compile comes after exactly the points native setThreshold arms from T and P, trunc(T - P), and not after a
// slice jitSoon would have armed. The test counts calls until jitcacheBodyEvents shows the compile and compares the call
// with the one those points give, read from the body's ucb.feedback. Nothing installs in that run. A plain build has
// no $vm or jitcacheBodyEvents: there every role evaluates the text untainted and checks nothing.
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const text = "(function llcBody(x) { return (x * 7 + 3) | 0; })";
    const canTaint = typeof $vm === "object" && $vm !== null && typeof $vm.runTaintedString === "function";
    const hasEvents = typeof jitcacheBodyEvents === "function";

    // The Producer evaluates the text untainted; every other role, the oracle's Off runs included, runs the Consumer's
    // path, tainted where it can be.
    const evaluate = () => (t.produces || !canTaint ? loadString(text) : $vm.runTaintedString(text));

    let total = 0;
    if (t.produces) {
        const early = evaluate();
        const compiling = evaluate();
        // `early` gets its CodeBlock before the compile and so keeps running in the LLInt afterwards.
        total = (total + early(1)) | 0;
        for (let i = 0; i < 60; ++i)
            total = (total + compiling(i)) | 0;
        if (t.sequence === 1) {
            for (let i = 0; i < 3; ++i)
                total = (total + early(i)) | 0;
            // While `compiling` still holds its baseline CodeBlock; delta at exit does the same in a plain build.
            t.captureDelta();
        }
        t.check(Number.isInteger(total), `llcBody's calls added up to ${total}`);
        print("llcBody ran in every role");
        t.finish();
        return;
    }

    const fn = evaluate();
    let compiledAt = 0;
    for (let call = 1; call <= 40; ++call) {
        total = (total + fn(call)) | 0;
        if (hasEvents && !compiledAt && jitcacheBodyEvents(fn, "call")?.baselineCompiles)
            compiledAt = call;
    }
    t.check(total === 7 * 820 + 3 * 40, `llcBody's 40 calls added up to ${total}`);
    print("llcBody ran in every role");

    if (!(t.twins && t.imports && canTaint)) {
        t.finish();
        return;
    }
    const feedback = ucbSections.feedback(t.readSection(t.bodyKey(fn), "ucb.feedback"));
    const threshold = ucbSections.i32(feedback.bytes, 40);
    const totalCount = new Float32Array(new Uint32Array([ucbSections.u32(feedback.bytes, 44)]).buffer)[0];
    const progress = totalCount + ucbSections.i32(feedback.bytes, 48);
    t.check(progress !== Math.trunc(progress), `the captured progress ${progress} is not fractional`);
    if (t.sequence === 1)
        t.check(progress > 40, `the captured progress ${progress} does not include the LLInt calls after the compile`);

    // armLLIntCounter (section 5.5): m_counter = -trunc(min(T - P, maximumExecutionCountsBetweenCheckpointsForBaseline)),
    // and the LLInt adds 5 at each prologue and 10 at each return, compiling at the first addition that reaches zero.
    const maximumBetweenCheckpoints = 1000;
    const slice = Math.trunc(Math.min(threshold - progress, maximumBetweenCheckpoints));
    let expected = 0;
    for (let call = 1, points = 0; !expected; ++call) {
        points += 5;
        if (points >= slice) {
            expected = call;
            break;
        }
        points += 10;
        if (points >= slice)
            expected = call;
    }
    t.check(compiledAt === expected, `llcBody compiled during call ${compiledAt}, and T ${threshold} with P ${progress} cross during call ${expected}`);
    t.finish();
})(...arguments);
