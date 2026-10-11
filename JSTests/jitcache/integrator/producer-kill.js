// jitcache-runs: Producer --jitcache-test-kill=before-create@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-create@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=mid-stream@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-stream@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-envelope@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-reread@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-rename@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-runs: Producer --jitcache-test-kill=after-reread@2; Maintenance clean $ARTIFACT; Consumer
// jitcache-expect-exit: 0 kill
// jitcache-check: 0:1 resources/clean-no-temporary.ts
// jitcache-check: 1:1 resources/clean-one-temporary.ts
// jitcache-check: 2:1 resources/clean-one-temporary.ts
// jitcache-check: 3:1 resources/clean-one-temporary.ts
// jitcache-check: 4:1 resources/clean-one-temporary.ts
// jitcache-check: 5:1 resources/clean-one-temporary.ts
// jitcache-check: 6:1 resources/clean-no-temporary.ts
// jitcache-check: 7:1 resources/clean-one-temporary.ts

// SPEC-integrator.md section 15.2, producer-kill.js: one sequence per kill point of harness sub-SPEC section 12, each a
// Producer killed at its writer's second commit, then a clean, then a Consumer. The kill point counts every commit the
// writer starts, so between the first commit and the second the Producer runs only its own loops and native functions:
// a function with a recorded key that compiled there, a helper of resources/integrator.js or a builtin one calls, would
// be committed at its compilation and take the second commit's place (resources/integrator.js says when one compiles).
// - Sequences 0 to 6: pkFirst compiles first, and its commit is the first; pkSecond's is the second, which the kill
//   interrupts. The Consumer imports pkFirst, committed before the kill. pkSecond's key holds no body after the six
//   points before the rename, so the Consumer compiles it natively, and the new body after-rename, which it imports.
// - Sequence 7: the second commit recaptures pkFirst. After its compilation and first commit, pkFirst runs on in
//   baseline code, where its two property ICs gain cases and its baseline counter makes progress, so its capture beats
//   that body at the jitcacheDelta() that follows, and the kill interrupts the recapture after the reread. Before the
//   delta the Producer checks that pkFirst's commit was its only one, which leaves pkFirst the delta's only candidate,
//   and saves the bytes of every section of the body in the scratch directory. The Consumer imports the body and finds
//   those bytes in it.
// The clean after each kill removes the commit's temporary, one for the points from after-create to after-reread and
// none for before-create and after-rename, which the checkers read in its summary. A killed run writes no status and no
// heap description, and its oracle run, which has no kill point, runs to the end, so the script prints nothing (harness
// sub-SPEC section 7.6).
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const sectionNames = ["ucb.identity", "ucb.core", "ucb.feedback", "image.baseline", "baked-facts.baseline", "image-twins.baseline", "cb.state", "cb.summary", "ICsBaseline"];
    const afterRename = sequence === "6";
    const recapture = sequence === "7";

    function pkFirst(point) {
        return point.x * 4 + point.y;
    }

    function pkSecond(value) {
        return (value * 9 + 4) | 0;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total = (total + pkFirst({ x: i, y: 3 })) | 0;

    if (recapture) {
        for (let i = 0; i < 6; ++i)
            total = (total + pkFirst({ x: i, y: 3 })) | 0;
        checkSame(total, 3318, "pkFirst's total");
        if (role === "Producer") {
            checkSame(jitcacheProgress().capturesCommitted, 1, "the Producer's commits before the delta, which should be pkFirst's alone");
            const key = bodyKey(pkFirst);
            for (let i = 0; i < sectionNames.length; ++i) {
                const bytes = jitcacheReadSection(key, sectionNames[i]);
                if (bytes === null || writeFile(`${scratch}/producer-kill.${sectionNames[i]}`, bytes) !== bytes.byteLength)
                    fail(`cannot save the ${sectionNames[i]} section of pkFirst's body`);
            }
            jitcacheDelta();
            fail("the Producer returned from the delta whose commit its kill point interrupts");
        }
        if (role === "Consumer") {
            expectInstalled(pkFirst);
            const key = bodyKey(pkFirst);
            for (const name of sectionNames) {
                const saved = readFile(`${scratch}/producer-kill.${name}`, "binary");
                checkSameBytes(jitcacheReadSection(key, name), saved, `pkFirst's ${name} section after the kill interrupted its recapture`);
            }
        }
        return;
    }

    for (let i = 0; i < 40; ++i)
        total = (total + pkSecond(i)) | 0;
    checkSame(total, 10420, "pkFirst's and pkSecond's total");
    if (role === "Producer")
        fail("the Producer outlived the commit its kill point interrupts");
    if (role !== "Consumer")
        return;

    expectInstalled(pkFirst);
    if (afterRename) {
        expectInstalled(pkSecond);
        return;
    }
    check(!hasBody(pkSecond), "pkSecond's key holds a body the kill before the rename should have kept out");
    expectCompiledNatively(pkSecond);
})(...arguments);
