// jitcache-host: bun

// A module whose UCB is private to its executable, and a CommonJS wrapper compiled on a GraphCommonJSSourceProvider:
// both come from a Bun.ModuleGraph with `globals`, whose loader gives an ES module a module scope of its own, so the
// CodeCache neither serves nor keeps that module's UCB, and wraps a CommonJS module's provider in one whose hash mixes
// in the graph's overlay. The host imports resources/private-module-bun-module.mjs and requires
// resources/private-module-bun-cjs.js and runs their functions until they compile; then a graph imports both. The graph's
// module requests its body at the shared module's key, and its wrapper at the key of the wrapped provider's text, so in
// the Consumer both import from the bodies the host's captured (statistics), and the graph's functions import at their
// first calls, pmbRead's inside the module's load. The graph's functions resolve pmbBase in its globals, where the
// host's find no such global, so an image installs only where its baked facts match the CodeBlock it installs on, which
// the twin check compares at each install.
// The app, resources/private-module-bun-app.js, makes the wrapper's checks in a `bun build --compile` executable, where
// the module carries the source digest the build recorded and the graph's load computes none from text.
// Every run, the app's output included, equals the JITCache-off run.
//
// The runs use the lane's default sequence, so the test's own VM calls delta at exit in the Producer, which captures the
// module's top level and the wrapper, both of which run once; the app gets the same through its flags.
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const moduleURL = pathToFileURL(path.join(__dirname, "resources", "private-module-bun-module.mjs")).href;
const cjsPath = path.join(__dirname, "resources", "private-module-bun-cjs.js");

async function main()
{
    const hostModule = await import(moduleURL);
    const hostCjs = require(cjsPath);
    let total = hostModule.pmbStart;
    for (let i = 0; i < 400; ++i)
        total = (total + hostModule.pmbRead(i) + hostCjs.pmbCount(i)) | 0;
    console.log(`host ${total} ${hostCjs.calls()}`);

    // A graph's first import runs Bun's module loader and some of Bun's own builtins for the first time; a graph of its
    // own does that first, so that the windows below count the modules under test. Each load's statistics are read around
    // the load itself, since a closure would make a request of its own.
    const warmGraph = new Bun.ModuleGraph({ globals: { pmbBase: 3 } });
    await warmGraph.import(pathToFileURL(path.join(__dirname, "resources", "warm-module.js")).href);
    warmGraph.dispose();
    // In the Consumer each load imports two bodies: the module's top level and pmbRead, which it calls; the wrapper's
    // program and the wrapper function it evaluates to. pmbRead and the wrapper function are children of a UCB that
    // carries the shared key whether it was imported or generated, so they import either way, and one import would
    // pass even if the graph's module or wrapper program had missed.
    const loadExpectations = t.imports ? { imports: [">=", 2] } : { };
    const graph = new Bun.ModuleGraph({ globals: { pmbBase: 3 } });
    let before = t.statistics();
    const graphModule = await graph.import(moduleURL);
    let after = t.statistics();
    t.expect("the graph's module", t.delta(before, after), loadExpectations);
    before = t.statistics();
    const graphCjs = (await graph.import(pathToFileURL(cjsPath).href)).default;
    after = t.statistics();
    t.expect("the graph's CommonJS module", t.delta(before, after), loadExpectations);
    t.check(graphModule.pmbRead !== hostModule.pmbRead && graphCjs.pmbCount !== hostCjs.pmbCount, "the graph shared the host's modules");

    // pmbRead's first call is the module's top level, inside the module's load; pmbCount's is its own.
    before = t.statistics();
    total = graphCjs.pmbCount(1);
    after = t.statistics();
    t.expect("the graph's pmbCount's first call", t.delta(before, after), t.imports ? { imports: [">=", 1] } : { });
    total = (total + graphModule.pmbStart) | 0;
    for (let i = 0; i < 400; ++i)
        total = (total + graphModule.pmbRead(i) + graphCjs.pmbCount(i)) | 0;
    console.log(`graph ${total} ${graphCjs.calls()}`);
    graph.dispose();

    const entries = ["private-module-bun-app.js", "private-module-bun-cjs.js", "warm-module.js"].map(name => path.join(__dirname, "resources", name));
    const app = t.buildApp("private-module-bun-app", entries, { format: "cjs" });
    t.runApp(app, { flags: t.produces ? ["--jitcache-delta-at-exit"] : [] });
    t.finish();
}

main().catch(error => {
    console.log(`failed: ${error}`);
    process.exitCode = 1;
});
