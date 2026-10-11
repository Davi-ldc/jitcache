// jitcache-runs: Producer --jitcache-test-writer-fault=writer.create@2; Consumer
// jitcache-runs: Producer --jitcache-test-writer-fault=writer.write@2; Consumer
// jitcache-runs: Producer --jitcache-test-writer-fault=writer.reread@2; Consumer
// jitcache-runs: Producer --jitcache-test-writer-fault=writer.publish@2; Consumer
// jitcache-expect-fault: 0 writer.create
// jitcache-expect-fault: 0 writer.write
// jitcache-expect-fault: 0 writer.reread
// jitcache-expect-fault: 0 writer.publish

// SPEC-integrator.md section 15.2, writer-faults.js: one sequence per writer fault hook of container sub-SPEC section
// 8.3, set in the Producer for its writer's second commit. wfFirst compiles first and is that writer's first commit;
// wfSecond compiles next, and its commit fails at the sequence's step. The recording fault ends production and leaves
// activity on (section 4.2), so the Producer's status names the step, wfFirst's body stays readable, and wfSecond's key
// holds no body. The Consumer installs wfFirst, which the fault left importable, and finds no body for wfSecond, which
// it compiles natively. The runner compares every output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const steps = ["writer.create", "writer.write", "writer.reread", "writer.publish"];

    function wfFirst(point) {
        return point.x * 2 + point.y;
    }

    function wfSecond(value) {
        return (value * 13 + 7) | 0;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total = (total + wfFirst({ x: i, y: 5 })) | 0;
    for (let i = 0; i < 40; ++i)
        total = (total + wfSecond(i)) | 0;
    print(`wfFirst and wfSecond added up to ${total}`);

    if (role === "Producer") {
        checkSame(jitcacheStatus(), steps[Number(sequence)], "the Producer's fault once its second commit failed");
        check(hasBody(wfFirst), "wfFirst's commit, the one before the fault, left no body");
        check(!hasBody(wfSecond), "the commit that failed published a body for wfSecond");
        return;
    }

    if (role === "Consumer") {
        expectInstalled(wfFirst);
        check(!hasBody(wfSecond), "wfSecond's key holds a body the failed commit should not have published");
        expectCompiledNatively(wfSecond);
    }
})(...arguments);
