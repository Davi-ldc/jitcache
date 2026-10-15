// A module body imports in the Consumer at its request point (statistics, imports) and passes its twins, and so does a
// function the module declares. The module's top-level loop makes its body
// reach the baseline in the Producer. The module's own request runs while the module is instantiated, before this code,
// so the Consumer finds the import already counted when main runs.
//
// The runs use the lane's default sequence, Producer --jitcache-delta-at-exit; Consumer. Everything the module leaves
// in its environment is the same in every role.
load("./resources/ucb.js", "caller relative");

function scaled(value)
{
    return (value * 13 + 5) | 0;
}

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    // The reads surround the call alone: t.check, a body that ran in the Producer, has a body of its own under the default
    // delta capture and would import between them at its first call.
    const before = t.statistics();
    let total = scaled(1);
    const after = t.statistics();
    if (t.imports && before)
        t.check(before.imports >= 1 && before.records.Imported >= 1, () => `the module body did not import before it ran: ${t.describe(before)}`);
    if (t.imports)
        t.expect("the module's function", t.delta(before, after), { imports: 1 });
    for (let i = 0; i < 400; ++i)
        total = (total + scaled(i)) | 0;
    print(`function ${total}`);
})(...globalThis.arguments);

let moduleTotal = 0;
for (let i = 0; i < 3000; ++i)
    moduleTotal = (moduleTotal + i * 11) | 0;
print(`module ${moduleTotal}`);
ucbTest(...globalThis.arguments).finish();
