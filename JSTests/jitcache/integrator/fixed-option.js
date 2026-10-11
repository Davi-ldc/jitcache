// jitcache-runs: Producer --useLOLJIT=true
// jitcache-check: 0 resources/rejected-fixed-option.ts

// SPEC-integrator.md section 15.2, fixed-option.js. useLOLJIT is a fixed option whose required value is false
// (options.md): with it on, a baseline plan would compile with the LOL JIT. start checks the fixed options before it
// touches the artifact and rejects the Producer at start.fixed-option, with a detail naming useLOLJIT, which the checker
// reads in the start line. The VM stays unconfigured, and the runner compares the output with the JITCache-off run's,
// which runs with the same option. Nothing here runs often enough to compile.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function foBody(value) {
        return (value * 2 + 9) | 0;
    }

    let total = 0;
    for (let i = 0; i < 5; ++i)
        total = (total + foBody(i)) | 0;
    print(`foBody added up to ${total}`);

    if (role === "Producer") {
        checkSame(jitcacheStartOutcome(), "rejected", "the start of a Producer whose useLOLJIT is on");
        checkSame(jitcacheStatus(), null, "the fault of a Producer whose start was rejected");
    }
})(...arguments);
