// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --verboseOSR=true
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --verboseOSR=true
// jitcache-check: 1 resources/floor-check.ts

// SPEC-cb.md section 11.3, the floor of section 5.3. floorBody neither loops nor recurses and reads one property of one
// structure, so it has no polymorphic site and its counter travels. Its Producer runs it to the last call before its
// counter would cross: a probe with the same text, called until it compiles, gives that count. The delta at exit
// captures it there, with progress just short of its threshold T. In twins builds the Producer runs delta itself first
// and writes what it committed to <scratch>/cb-floor.json.
//
// In the second sequence a twins Producer then rewrites the committed counter with jitcacheRewriteSection to m_counter
// 0 and a total of T + 400, progress well past the threshold. No native capture holds that counter. With concurrent
// JIT off, baseline code enters operationOptimize at the entry increment that ends the slice armed last, and that slice
// ends at M·T, where the memory multiplier M is a few thousandths above 1 with the default pool. A DFG plan that
// finishes while a capture reads only zeroes m_counter at that end (section 4.1), so even that race leaves at most
// about M·T plus one entry increment. Strict validation accepts the rewritten counter, since V15 and S3 bound only its
// signs. Plain builds have neither jitcacheDelta nor jitcacheRewriteSection, so there the second sequence repeats the
// first and the checker finds no cb-floor.json. Their runs, like the first sequence of a twins build, cover progress
// just short of the threshold, which takes the same crossed branch in finishCounter as the rewritten progress: that
// check crosses from M·T - min(T, C) / 2 on, with the Consumer's multiplier and slice ceiling C (N4). floorBody makes
// no call after the script's delta and sees one type at each profile, so the delta at exit finds no capture of it that
// beats the committed body, rewritten or not, and leaves that body in place (THREAD Capture).
//
// In the Consumer the imported counter starts at the floor, two entry increments short of crossing, so floorBody enters
// operationOptimize on its second invocation and not during its first; resources/floor-check.ts reads the line
// operationOptimize writes on entry under verboseOSR. The script writes a marker on stderr before each invocation. The
// Producer, whose floorBody must stay one call short of crossing, makes the same three calls on the probe, which
// returns the same values.
(function main(role, scratch, artifact, sequence) {
    function floorBody(o) {
        return o.a * 2 + 1;
    }
    function floorProbe(o) {
        return o.a * 2 + 1;
    }
    noInline(floorBody);
    noInline(floorProbe);
    const input = i => ({ a: i });

    if (role === "Producer") {
        let calls = 0;
        while (!numberOfDFGCompiles(floorProbe) && calls < 100000)
            floorProbe(input(calls++));
        if (!numberOfDFGCompiles(floorProbe))
            throw new Error("floorProbe never reached the DFG");
        for (let i = 0; i < calls - 1; ++i)
            floorBody(input(i));
        if (numberOfDFGCompiles(floorBody))
            throw new Error("floorBody reached the DFG in the Producer");

        if (typeof jitcacheDelta === "function") {
            jitcacheDelta();
            const sections = load("./resources/cb-sections.js", "caller relative");
            const body = sections.checkBody(floorBody, "call", "floorBody");
            const header = body.state.header;
            if (header.counterMode !== sections.CounterMode.Carried)
                throw new Error("floorBody's committed counter does not travel");
            const threshold = header.counterActiveThreshold;
            let progress = header.counterTotalCount + header.counterValue;
            const rewritten = sequence === "1";
            if (rewritten) {
                progress = threshold + 400;
                sections.rewriteCounter(body, 0, progress);
                const after = sections.checkBody(floorBody, "call", "floorBody after the rewrite").state.header;
                if (after.counterValue !== 0 || after.counterTotalCount !== progress || after.counterActiveThreshold !== threshold)
                    throw new Error(`the rewrite left ${sections.describe(after)}`);
            }
            writeFile(`${scratch}/cb-floor.json`, JSON.stringify({ rewritten, threshold, progress }));
        }
    }

    const call = role === "Producer" ? floorProbe : floorBody;
    const results = [];
    for (let invocation = 1; invocation <= 3; ++invocation) {
        printErr(`cb-floor invocation ${invocation}`);
        results.push(call(input(invocation)));
    }
    print(results.join(" "));
})(...arguments);
