// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-delta-capture=compiled; Consumer
// jitcache-expect-no-install: 1

// SPEC-ucb.md section 13.3, run-once-module.mjs: a module's top level runs once and never compiles, and so does a
// function it calls twice. With the delta capture all, delta captures both without an image, and the Consumer imports
// the module at its request point, before its code runs, and the function at its first call, with no install: their
// CodeBlocks start in the LLInt. With the delta capture compiled neither has a body, and the Consumer generates both.
// Nothing compiles in any run, so no Consumer installs anything.
load("./resources/ucb.js", "caller relative");

function moduleHelper(text)
{
    return text.length * 3;
}

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const scoped = t.sequence === 0;
    // Statistics are read around the call itself, before any helper that has not run yet, since each helper is a body that
    // may have a body too.
    const before = t.statistics();
    let total = moduleHelper("ab");
    const after = t.statistics();
    total += moduleHelper("cde");
    if (t.imports && before) {
        const imported = before.records.Imported >= 1;
        t.check(scoped ? imported : !imported, () => `the module body ${scoped ? "did not import" : "imported"} before it ran: ${t.describe(before)}`);
    }
    t.expect("the module's function", t.delta(before, after), { imports: t.imports && scoped ? 1 : 0 });
    if (t.imports && t.twins) {
        const events = t.events(moduleHelper);
        t.check(events && events.llintInstructions > 0 && !events.baselineCompiles, () => `moduleHelper did not run in the LLInt: ${t.describe(events)}`);
    }
    print(`module function ${total}`);
})(...globalThis.arguments);

let moduleTotal = 0;
for (let i = 0; i < 3; ++i)
    moduleTotal += i * 11;
print(`module ${moduleTotal}`);
ucbTest(...globalThis.arguments).finish();
