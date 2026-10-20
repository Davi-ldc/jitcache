// jitcache-runs: Off --diskCachePath=.; Producer --diskCachePath=. --jitcache-delta-at-exit; Consumer --diskCachePath=.
// jitcache-runs: Off --diskCachePath=.; Producer --diskCachePath=. --jitcache-delta-at-exit; ConsumerProducer --diskCachePath=. --jitcache-delta-at-exit
// jitcache-runs: Off --diskCachePath=.; Producer --diskCachePath=. --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 0:2
// jitcache-expect-no-install: 1:2
// jitcache-expect-no-install: 2:2
// jitcache-pin: off its runs decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and generates where this build decodes

// Code the native path decodes from a bytecode payload gets no record and no identity, so nothing in it imports or is
// captured. The payload is the jsc shell's bytecode cache (--diskCachePath; every run works in its sequence's scratch
// directory), which the first run of the first three sequences writes with JITCache off, since a shell provider writes
// its cache when it is destroyed: it loads the program in a realm of its own, calls DcF, whose child dcChild runs too,
// and calls DcG without constructing it, so DcG's construct slot is not cached. A decoding run cannot add to that
// payload, since the decoder records no executable an update could patch, so every later run decodes the same bytes.
// - Sequences 0 and 1: the Producer, and then a Consumer or a ConsumerProducer, decode the program and the call slots
//   of DcF, dcChild and DcG. Across the program's load and each first call, no record is made and nothing imports or
//   attaches (statistics), and no decoded function has a key (jitcacheBodyKey). DcG's first construction generates its
//   construct slot, which records nothing and, in a run that imports, misses NoKey.
// - Sequence 2: the Producer decodes, its delta at exit commits nothing for the decoded code, so the Consumer, which
//   generates the same program without the cache, misses NoBody at the program and at each function.
// - Sequence 3: without the cache, the Producer generates and captures the same program and functions, and the
//   Consumer imports each of them (statistics).
// The decoding consumers run no other decoded code and may install nothing. Every run prints the same results, which
// the oracle compares with the JITCache-off run. The first run tells itself from the oracle's Off runs by a file it
// leaves in the scratch directory.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const cached = t.sequence <= 2;
    const writesCache = t.off && cached && !t.hasFile("decoded-code.cache-written");
    // The runs that read the payload; the Consumer of sequence 2 and both runs of sequence 3 generate.
    const decodes = cached && !(t.sequence === 2 && t.role === "Consumer");

    const program = `
        function DcF(x) {
            function dcChild(y) { return y + 1; }
            return dcChild(x) * 2;
        }
        function DcG(x) {
            if (new.target) {
                this.g = x;
                return;
            }
            return x * 3;
        }
    `;
    const realm = createGlobalObject();

    if (writesCache) {
        t.writeText("decoded-code.cache-written", "the first run wrote the bytecode cache");
        realm.loadString(program);
        realm.DcF(1);
        realm.DcG(1);
        print("the first run wrote the bytecode cache");
        return;
    }

    // Statistics are read around the calls themselves: this helper's own request comes before its first read, while an
    // action passed as a closure would make a request of its own inside the window.
    function observe(label, fn, x, construct, expectations)
    {
        const before = t.statistics();
        const value = construct ? new fn(x) : fn(x);
        const after = t.statistics();
        t.expect(label, t.delta(before, after), expectations);
        return value;
    }

    // What each request leaves, by the kind of run.
    const nothing = { "records.Generated": 0, "records.Imported": 0, imports: 0, attaches: 0, "misses.NoKey": 0, "misses.NoBody": 0, sourceDigests: 0, contextDigests: 0 };
    let programLoad;
    let firstCall;
    let construction;
    if (decodes) {
        programLoad = nothing;
        firstCall = nothing;
        construction = { ...nothing, "misses.NoKey": t.imports ? 1 : 0 };
    } else if (t.imports && cached) {
        // The Consumer of sequence 2: no body at any key.
        programLoad = { imports: 0, "misses.NoBody": 1, "records.Generated": 1 };
        firstCall = { imports: 0, "misses.NoBody": [">=", 1] };
        construction = { imports: 0, "misses.NoBody": 1 };
    } else if (t.imports) {
        programLoad = { imports: 1, "records.Imported": 1 };
        firstCall = { imports: [">=", 1] };
        construction = { imports: 1 };
    } else {
        programLoad = { "records.Generated": 1, imports: 0 };
        firstCall = { imports: 0 };
        construction = { imports: 0 };
    }

    observe("the program's load", realm.loadString, program, false, programLoad);
    const results = [
        observe("DcF's first call", realm.DcF, 1, false, firstCall),
        observe("DcG's first call", realm.DcG, 1, false, firstCall),
        observe("DcG's first construction", realm.DcG, 2, true, construction).g,
    ];
    if (decodes) {
        for (const [name, fn] of [["DcF", realm.DcF], ["DcG", realm.DcG]])
            t.check(t.bodyKey(fn) === null, `the decoded ${name} has a key`);
        t.check(t.bodyKey(realm.DcG, "construct") === null, "the construct slot generated on the decoded DcG has a key");
    }

    let total = 0;
    for (let i = 0; i < 400; ++i)
        total = (total + realm.DcF(i) + realm.DcG(i) + new realm.DcG(i).g) | 0;
    print(`${results.join(" ")} ${total}`);
    t.finish();
})(...arguments);
