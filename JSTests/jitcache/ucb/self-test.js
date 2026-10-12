// jitcache-runs: Producer; Consumer; Consumer --jitcache-strict=0; ConsumerProducer; Consumer
// jitcache-requires: twins
// jitcache-expect-no-install: 1
// jitcache-expect-no-install: 2
// jitcache-expect-no-install: 3
// jitcache-expect-no-install: 4
// jitcache-expect-fault: 4 ucb.supplied-digest
// jitcache-expect-twin: 4 difference ucb

// SPEC-ucb.md section 13.1: $vm.jitCacheUCBSelfTest() passes in one run of each configuration, Producer, Consumer with
// strict on, Consumer with strict off and ConsumerProducer, in that order; the last Consumer runs the InvalidMaterial
// scope, which leaves cache activity off at ucb.supplied-digest and T7's two differences in its twin report.
//
// The runs differ only in their options, which a script cannot read, so each run counts the runs before it in a file
// of the sequence's scratch directory. The Off runs of the oracle run nothing and print the same line.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    if (t.configured) {
        const run = Number(t.readText("self-test.runs") ?? "0");
        t.writeText("self-test.runs", String(run + 1));
        t.check(t.status() === null, `the run starts with cache activity off at ${t.status()}`);
        if (run < 4) {
            $vm.jitCacheUCBSelfTest();
            t.check(t.status() === null, `the self-test of run ${run} left a fault at ${t.status()}`);
        } else {
            $vm.jitCacheUCBSelfTest({ invalidMaterial: true });
            t.check(t.status() === "ucb.supplied-digest", `the InvalidMaterial scope left the fault at ${t.status()}, expected ucb.supplied-digest`);
        }
    }
    print("the UCB self-test ran in this configuration");
    t.finish();
})(...arguments);
