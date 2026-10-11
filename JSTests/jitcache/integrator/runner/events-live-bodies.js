// jitcache-runs: Producer --jitcache-body-events=live-body-events.jsonl
// jitcache-check: 0 resources/live-bodies.ts

// Harness sub-SPEC H6: a Producer run's body-event dump has a line for each keyed body still alive, with the counts
// jitcacheBodyEvents gave for it at the end. h6Hot compiles and h6Cold stays in the LLInt; a global keeps both alive
// through the dump, in every run alike. The Producer writes each function's key and final counts to its scratch
// directory for the checker; the oracle's Off run, which has no keys, writes nothing, so both print the same.
(function main(role, scratch) {
    function h6Hot(x) {
        return x * 2;
    }
    function h6Cold(x) {
        return x - 1;
    }
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total += h6Hot(i);
    total = h6Cold(total);
    print(total);
    globalThis.h6LiveBodies = [h6Hot, h6Cold];
    if (role !== "Producer")
        return;
    const bodies = globalThis.h6LiveBodies.map(fn => ({ name: fn.name, key: jitcacheBodyKey(fn, "call"), counts: jitcacheBodyEvents(fn, "call") }));
    writeFile(`${scratch}/live-bodies.json`, JSON.stringify(bodies));
})(...arguments);
