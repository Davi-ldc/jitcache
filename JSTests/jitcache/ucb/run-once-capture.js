// jitcache-runs: Producer --jitcache-delta-at-exit --forceCodeBlockToJettisonDueToOldAge=true; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-delta-capture=compiled; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitCacheImportLLIntProgress=false
// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-delta-at-exit; Consumer

// The bodies that run without compiling, which delta captures without an image. In the
// Producer, roSmall runs three times and roArrow twice, neither compiles, and roDead runs twice and loses its CodeBlock to
// a full collection before the capture (sequence 0, which jettisons every CodeBlock old enough at each collection).
// roHot compiles, so its body has an image.
// - Sequence 0: delta captures roSmall, roArrow and roDead without an image, roDead through the UCB registry; in the
//   Consumer each imports at its first call (statistics) with no install, its CodeBlocks start in the LLInt, and roSmall's
//   counter keeps the 45 points the Producer left: its baseline compile comes at the call that the carried progress
//   gives, earlier than a JITCache-off run's.
// - Sequence 1, with the delta capture compiled: none of them has a body, and the Consumer generates them.
// - Sequence 2, with jitCacheImportLLIntProgress off in the Consumer: roSmall imports, and its counter arms from zero, so
//   it compiles at the JITCache-off run's call.
// - Sequence 3: the ConsumerProducer imports roSmall without an image, runs it until it compiles natively, and that
//   compile's capture replaces the body with one that has an image, which the last Consumer installs. roHot keeps its
//   image across both deltas.
// The twin check compares every import's counter with the progress it restores. The oracle compares every run with a
// JITCache-off run, so every role prints the same lines.
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);

    function roSmall(a, b) { return (a + b * 2) | 0; }
    const roArrow = text => text.length;
    function roDead(x) { return (x * 3) | 0; }
    function roHot(x) { return (x * 7 + 1) | 0; }

    let total = 0;
    for (let i = 0; i < 400; ++i)
        total = (total + roHot(i)) | 0;

    // The Producer's paths of every role but the ConsumerProducer of sequence 3; a JITCache-off run takes the consumer's
    // path, which prints the same lines.
    const capturing = t.role === "Producer";
    if (capturing) {
        for (let i = 0; i < 3; ++i)
            total = (total + roSmall(i, 1)) | 0;
        total = (total + roArrow("ab") + roArrow("cde")) | 0;
        total = (total + roDead(1) + roDead(2)) | 0;
        const keys = { roSmall: t.bodyKey(roSmall), roArrow: t.bodyKey(roArrow), roDead: t.bodyKey(roDead), roHot: t.bodyKey(roHot) };
        if (t.twins) {
            t.writeText(`run-once-${t.sequence}.keys`, JSON.stringify(keys));
            if (t.sequence === 0) {
                fullGC();
                t.check(t.events(roDead) === null, "roDead kept its CodeBlock through a full collection");
            }
            const result = t.captureDelta();
            const imageless = t.sequence !== 1;
            // roHot's finalize capture already holds what it learned, so with the delta capture compiled the delta may commit
            // nothing.
            if (imageless)
                t.check(result.committedBodies >= 1, () => `the delta committed ${t.describe(result)}`);
            for (const name of ["roSmall", "roArrow", "roDead"]) {
                const feedback = t.readSection(keys[name], "ucb.feedback");
                t.check(imageless ? feedback !== null : feedback === null, `${name}'s body after the delta: ${feedback === null ? "none" : "one"}`);
                t.check(t.readSection(keys[name], "image.baseline") === null, `${name}'s body holds an image`);
            }
            t.check(t.readSection(keys.roHot, "image.baseline") !== null, "roHot's body holds no image");
        }
        print(`run-once ran in every role`);
        t.finish();
        return;
    }

    // The consumer's path: roSmall, roArrow and roDead import at their first calls when the artifact holds their bodies.
    // Statistics are read around the calls themselves, since a window's own action is a body that may have a body too.
    const keys = t.twins ? JSON.parse(t.readText(`run-once-${t.sequence}.keys`) ?? "null") : null;
    const imported = t.imports && t.sequence !== 1;
    // The last Consumer of sequence 3 finds roSmall's body with an image, which the ConsumerProducer's compile left.
    const roSmallHasImage = t.role === "Consumer" && t.sequence === 3;
    // The body the run starts from, read before anything runs, since a ConsumerProducer's own compile replaces it.
    const startingFeedback = t.twins && keys && t.imports && t.sequence !== 1 ? t.readSection(keys.roSmall, "ucb.feedback") : null;
    // Every helper the reads below call has run before the first read, so only the three requests fall between them.
    const installsBefore = t.progress()?.installs;
    const atStart = t.statistics();
    total = (total + roSmall(0, 1)) | 0;
    const afterRoSmall = t.statistics();
    const installsAfterRoSmall = t.progress()?.installs;
    total = (total + roArrow("ab")) | 0;
    const afterRoArrow = t.statistics();
    total = (total + roDead(1)) | 0;
    const afterRoDead = t.statistics();
    t.expect("roSmall's first call", t.delta(atStart, afterRoSmall), { imports: imported ? 1 : 0 });
    t.expect("roArrow's first call", t.delta(afterRoSmall, afterRoArrow), { imports: imported ? 1 : 0 });
    t.expect("roDead's first call", t.delta(afterRoArrow, afterRoDead), { imports: imported ? 1 : 0 });
    if (imported && t.twins) {
        t.check(t.progress().installs === installsAfterRoSmall, "a body without an image installed");
        if (!roSmallHasImage)
            t.check(installsAfterRoSmall === installsBefore, "roSmall's body without an image installed");
        for (const fn of roSmallHasImage ? [roArrow, roDead] : [roSmall, roArrow, roDead]) {
            const events = t.events(fn);
            t.check(events && events.llintInstructions > 0 && !events.baselineCompiles, () => `a run-once body did not start in the LLInt: ${t.describe(events)}`);
        }
    }

    // roSmall runs on until it compiles; the call the counter gives is the one the carried progress predicts.
    let compiledAt = 0;
    for (let call = 2; call <= 40; ++call) {
        total = (total + roSmall(call, 1)) | 0;
        if (!compiledAt && t.events(roSmall)?.baselineCompiles)
            compiledAt = call;
    }
    print(`run-once ran in every role`);

    if (startingFeedback) {
        const feedback = ucbSections.feedback(startingFeedback);
        const threshold = feedback.llintActiveThreshold;
        const progress = feedback.llintProgress;
        if (roSmallHasImage) {
            // The ConsumerProducer's compile replaced the body, which the last Consumer installs.
            t.check(t.readSection(keys.roSmall, "image.baseline") !== null, "the native compile's capture did not replace roSmall's body");
            t.check(installsAfterRoSmall > installsBefore, "the last Consumer did not install roSmall");
        } else {
            t.check(progress === 45, `roSmall's body carries ${progress} points, the Producer's three calls leave 45`);
            const carried = t.sequence === 2 ? 0 : Math.min(progress, threshold - 15);
            // armLLIntCounter arms trunc(min(T - P, 1000)) points; the LLInt adds 5 at each prologue and 10 at each return.
            const slice = Math.trunc(Math.min(threshold - carried, 1000));
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
            t.check(compiledAt === expected, `roSmall compiled during call ${compiledAt}, and T ${threshold} with ${carried} carried points cross during call ${expected}`);
        }
        if (t.role === "ConsumerProducer")
            t.check(t.readSection(keys.roHot, "image.baseline") !== null, "roHot's body lost its image");
    }
    t.finish();
})(...arguments);
