// jitcache-requires: twins
// jitcache-runs: Producer --jitcache-bench-report=module-analysis-shell.bench; Consumer

// The jsc shell captures a module's analysis but never imports one, as no provider of its asks. The Producer imports
// resources/module-analysis/ma-shell-a.mjs and the module it imports, whose top levels run once, then calls delta,
// which captures both without an image, each with the analysis its parse built. Their keys come from the generate
// events of the Producer's bench report, which delta flushed, and jitcacheReadSection reads each body's ucb.analysis,
// non-empty. The Consumer imports their UCBs, yet no analysis: analysisImports stays 0 across their load, and both are
// parsed. A function every run calls often enough to compile gives the Consumer a body to install. Every run equals the
// JITCache-off run.
load("./resources/ucb.js", "caller relative");

function shellHot(i)
{
    return (i * 7) & 255;
}

async function main(role, scratch, artifact, sequence)
{
    const t = ucbTest(role, scratch, artifact, sequence);
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total = (total + shellHot(i)) | 0;
    print(`hot ${total}`);

    const before = t.statistics();
    const shell = await import("./resources/module-analysis/ma-shell-a.mjs");
    const after = t.statistics();
    t.expect("the modules' import", t.delta(before, after), { analysisImports: 0, analysisMisses: 0, analysisAbsent: 0 });
    print(`shell ${shell.maShellA} ${shell.default()}`);

    if (!t.produces || !t.twins)
        return;
    t.captureDelta();
    const generated = t.readText("module-analysis-shell.bench").split("\n").filter(line => line.length)
        .map(line => JSON.parse(line)).filter(event => event.event === "generate" && event.kind === "module");
    for (const name of ["ma-shell-a.mjs", "ma-shell-b.mjs"]) {
        const event = generated.find(candidate => typeof candidate.url === "string" && candidate.url.endsWith(name));
        t.check(event && event.key, () => `no generate event names ${name} with a key`);
        const analysis = t.readSection(event.key, "ucb.analysis");
        t.check(analysis && analysis.length > 0, () => `the body of ${name} holds no ucb.analysis`);
    }
}

await main(...globalThis.arguments);
ucbTest(...globalThis.arguments).finish();
