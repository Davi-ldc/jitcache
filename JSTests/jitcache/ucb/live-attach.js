// jitcache-runs: Producer; ConsumerProducer --useConcurrentJIT=false --codeBlockAgingLeaseMultiplier=0
// jitcache-runs: Producer --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --jitAllowlist=jitcacheLiveAttachAllowsNothing
// jitcache-expect-no-install: 0:1
// jitcache-expect-no-install: 1:1

// A request that meets a live UCB, at a CodeCache hit or at a filled UFE slot, attaches a pending import to it only
// when that pays.
//
// Sequence 0. The Producer only creates the artifact. In the ConsumerProducer a function laF, declared by a program
// evaluated in realms of its own (runString), and a looping program L, evaluated with loadString, are generated while
// the artifact holds no body for them, reach the baseline, and are captured and committed by this VM; their code is then
// parked in their UCBs' sharing slots.
// - While the code is parked, laF's filled slot in a new realm attaches nothing, and a loop of loadString(L) computes no
//   source digest after its first request (statistics), since parked code stops an attach first. Each of these windows
//   runs while a CodeBlock that uses the parked code is on the stack: laF's inside the last call of the realm whose laF
//   compiled the body, which laF makes through its `during` argument, and L's inside the evaluation that compiled L,
//   which calls laDuring() at its end. That CodeBlock holds a reference to the code, and
//   Heap::releaseUnusedSharedBaselineCode releases only code that its UCB alone holds, so no collection can release it
//   before the window's requests, whenever the collector runs (--collectContinuously=true included).
// - Once the parked code is released, the next request of each attaches, and its newborn CodeBlock installs the body
//   (statistics attaches, jitcacheProgress installs). Releasing parked code takes the death of every CodeBlock that uses
//   it and a full collection after that: this run gives the release no lease (--codeBlockAgingLeaseMultiplier=0,
//   Heap::releaseUnusedSharedBaselineCode), and the test drops the realms and collects. A collection can keep a dead
//   realm alive through a stale stack word, so each release is tried a few times.
// - L's attach computes no source digest, since the CodeCache serves its UCB only to its own text, and a later release
//   and attach at the same commit identifier re-checks nothing: neither computes a digest.
// The windows after a release read the statistics around the request alone, since the collection that releases the
// parked code also kills the CodeBlocks of this script's helpers that compiled, which attach at their next calls.
// - In twins runs the ConsumerProducer then rewrites laF's body so that its context differs, which commits it under a new
//   index token: the next request after a release misses Context and stamps that token, and a request at the same token
//   after the miss reads nothing (statistics misses.BodyUnchanged, and no digest).
// Sequence 1. The Producer captures laF and L; the Consumer runs with a jitAllowlist that names no function, so the
// shouldJIT gate drops each import at its install (statistics gateDrops), which stamps the index token as missed, and
// every later request at that token reads nothing (statistics misses.BodyUnchanged, and no digest).
// Neither the gate's Consumer nor a ConsumerProducer whose release a stale stack word kept from happening has to
// install anything, so both declare jitcache-expect-no-install; twins runs check the installs themselves.
//
// The runs print one line in every role and keep their results to assertions; nothing they create outlives them, and
// the global laDuring that L calls is deleted before the run ends.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const programText = "function laF(x, during) { let total = x; for (let i = 0; i < 4; ++i) total = (total * 5 + i) | 0; during(); return total; }";
    const loopText = "{ let laTotal = 0; for (let i = 0; i < 3000; ++i) laTotal = (laTotal + i * 3) | 0; laDuring(); laTotal; }";
    // Every value laF's profiles sample falls in a category its first call already sampled, so a later capture of laF,
    // such as the one that follows the native compile the CodeBlocks born in the LLInt after a miss reach, ties the
    // saved body: it is no richer, and a capture at a baseline compile lists no IC case and carries no counter
    // progress. A tie keeps the saved body, so that capture writes nothing and the window it falls in computes no
    // digest. laF's arithmetic sees only Int32s and never overflows, and each argument has one category: `this` is a
    // realm's global proxy; `during` is a function, a window's callback or, outside a window, a native one, which has
    // no body to request; and x starts at firstX, because 0 and 1 are SpecBoolInt32 and every other Int32 is
    // SpecNonBoolInt32. An argument profile keeps only its last sample until a collection, a baseline compile or a
    // tier-up check folds it into the prediction, whenever that happens, so an x of 0 or 1 would reach some captures
    // and miss others.
    const idle = Math.abs;
    const firstX = 2;
    const loopResult = 13495500;
    const laF = x => {
        let total = x;
        for (let i = 0; i < 4; ++i)
            total = (total * 5 + i) | 0;
        return total;
    };
    const attempts = t.observes ? 4 : 1;

    // What the next evaluation of L runs at its end. It runs once: the evaluations it makes find nothing to run.
    let duringLoop = null;
    globalThis.laDuring = () => {
        const action = duringLoop;
        duringLoop = null;
        if (action)
            action();
    };

    function installs()
    {
        const progress = t.progress();
        return progress ? progress.installs : 0;
    }

    // Calls laF of a new realm `count` times, with x from firstX up, and checks its results; the last call runs
    // `during` before it returns. The realm dies once this returns.
    function runInNewRealm(count, during = idle)
    {
        const realm = runString(programText);
        for (let i = 0; i < count; ++i) {
            const x = firstX + i;
            const value = realm.laF(x, i === count - 1 ? during : idle);
            t.check(value === laF(x), () => `laF(${x}) gave ${value}`);
        }
    }

    // Evaluates L, which runs `during` at its end.
    function runLoopText(during = null)
    {
        duringLoop = during;
        const value = loadString(loopText);
        t.check(value === loopResult, `the looping program gave ${value}`);
    }

    function collect()
    {
        for (let i = 0; i < 3; ++i)
            gc();
    }

    // Calls fn(a, b), which makes one request, with the statistics and the install count read around that call alone. A
    // collection that releases the parked code also kills the CodeBlocks of this script's helpers that compiled, whose
    // bodies this VM committed too, and each helper's next call makes a newborn CodeBlock that attaches. So the readers run
    // once first, and nothing else runs between the reads: the install count is read before t.delta runs, since t.delta's
    // difference compiles after enough windows and would install inside the count.
    function measured(fn, a, b)
    {
        t.statistics();
        installs();
        const installsBefore = installs();
        const before = t.statistics();
        const value = fn(a, b);
        const after = t.statistics();
        const installed = installs() - installsBefore;
        return { value, delta: t.delta(before, after), installed };
    }

    // laF's request: its first call in a new realm, a request for its filled slot.
    function laFRequest()
    {
        const realm = runString(programText);
        const result = measured(realm.laF, firstX, idle);
        t.check(result.value === laF(firstX), () => `laF(${firstX}) gave ${result.value}`);
        return result;
    }

    // L's request: its evaluation, a CodeCache hit.
    function loopRequest()
    {
        const result = measured(loadString, loopText);
        t.check(result.value === loopResult, `the looping program gave ${result.value}`);
        return result;
    }

    // Runs `request` until it matches its live UCB with the body, which it attaches to unless `misses` is given, the miss
    // the match ends in. Each attempt collects first, so that the code the UCB parked is released; an attempt that met
    // parked code made a CodeBlock that uses it, which the next attempt's collection kills.
    function afterRelease(label, request, expectations, misses = null)
    {
        for (let attempt = 0; attempt < attempts; ++attempt) {
            collect();
            const { delta, installed } = request();
            if (!delta)
                return;
            const matched = misses ? delta.misses[misses] : delta.attaches;
            if (!matched && attempt + 1 < attempts)
                continue;
            t.expect(label, delta, expectations);
            t.check(installed === (misses ? 0 : 1), `${label}: ${installed} bodies were installed`);
            return;
        }
    }

    function done()
    {
        delete globalThis.laDuring;
        print("live attach");
        t.finish();
    }

    if (t.sequence === 0) {
        if (t.role === "Producer") {
            done();
            return;
        }

        // Generated while the artifact has no body for them, then compiled, captured, committed and parked. Each window
        // runs while the CodeBlock that compiled its body is on the stack, so the code stays parked: neither request
        // attaches or computes a digest.
        runInNewRealm(100, () => t.window("laF's filled slot while its code is parked", () => runInNewRealm(3), {
            attaches: 0, sourceDigests: 0, contextDigests: 0, holderDigests: 0,
        }));
        runLoopText(() => t.window("loadString of the looping program while its code is parked", () => {
            for (let i = 0; i < 5; ++i)
                runLoopText();
        }, { attaches: 0, sourceDigests: 0, contextDigests: 0, holderDigests: 0 }));

        // Released: the first request of each attaches and installs. L's computes no source digest, since the CodeCache
        // serves its UCB only to its own text.
        afterRelease("laF's first request after the release", laFRequest, {
            attaches: 1, contextDigests: 1, holderDigests: 1, sourceDigests: 0,
        });
        afterRelease("the looping program's first request after the release", loopRequest, {
            attaches: 1, contextDigests: 1, sourceDigests: 0, holderDigests: 0,
        });

        // Released again: the attach at the matched commit identifier re-checks nothing.
        afterRelease("laF's attach at the matched commit identifier", laFRequest, {
            attaches: 1, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
        });
        afterRelease("the looping program's attach at the matched commit identifier", loopRequest, {
            attaches: 1, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
        });

        // A miss, then a request at the index token it was stamped with. In twins runs this VM rewrites the first byte
        // of laF's body's context digest (jitcacheRewriteSection), which commits the body under a new token, so the next
        // request after a release matches the live UCB again and misses Context, stamped; the request after that finds
        // the stamp current and reads nothing. The miss installs nothing,
        // so the UCB parks no code and the second request needs no release.
        if (t.twins) {
            const realm = runString(programText);
            realm.laF(firstX, idle);
            const key = t.bodyKey(realm.laF);
            const identity = t.readSection(key, "ucb.identity");
            t.check(identity, "laF has no ucb.identity");
            t.rewriteSection(key, "ucb.identity", 48, [identity[48] ^ 0x5a]);
        }
        afterRelease("laF's request after its body's context changed", laFRequest, {
            "misses.Context": 1, attaches: 0, contextDigests: 1, holderDigests: 0, sourceDigests: 0,
        }, "Context");
        t.expect("laF's request at the index token its miss was stamped with", laFRequest().delta, {
            "misses.BodyUnchanged": 1, "misses.Context": 0, attaches: 0, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
        });
        done();
        return;
    }

    // Sequence 1.
    if (t.role === "Producer") {
        runInNewRealm(100);
        runLoopText();
        done();
        return;
    }
    // The first window also imports the helpers the Producer ran hot, and the gate drops those too.
    t.window("laF's import, which the gate drops", () => runInNewRealm(3), { imports: [">=", 1], gateDrops: [">=", 1] });
    t.window("laF's filled slot after the drop", () => runInNewRealm(3), {
        "misses.BodyUnchanged": 1, attaches: 0, imports: 0, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
    });
    t.window("the looping program's import, which the gate drops", runLoopText, { imports: 1, gateDrops: 1 });
    t.window("the looping program's CodeCache hit after the drop", runLoopText, {
        "misses.BodyUnchanged": 1, attaches: 0, imports: 0, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
    });
    done();
})(...arguments);
