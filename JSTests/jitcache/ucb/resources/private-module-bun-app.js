// The app of private-module-bun.js, built with `bun build --compile --format=cjs` together with private-module-bun-cjs.js
// and warm-module.js, which it loads at run time from beside it in the executable, so that the module keeps the source
// digest the build recorded, and run as a JITCache VM of its own with the run's role (resources/ucb-bun.js, runApp). The host requires the
// module and runs it hot; then a Bun.ModuleGraph with `globals` imports it, which compiles its wrapper on a
// GraphCommonJSSourceProvider. That provider supplies the digest of the module's provider, so the graph's load computes
// no source digest from text (statistics sourceDigests), and in the Consumer the wrapper imports the body the host's
// wrapper left (statistics). The app's output, which private-module-bun.js prints, equals the JITCache-off run.
const path = require("node:path");
const { pathToFileURL } = require("node:url");
const { appContext } = require("./ucb-bun.js");

const t = appContext();
// The embedded module sits beside the entry module in the executable, whose path is Bun.main. A bundle inlines
// `__dirname` as the directory the app was built from, so path.join(__dirname, ...) would load its source from disk.
const cjsPath = path.join(path.dirname(Bun.main), "private-module-bun-cjs.js");
const warmPath = path.join(path.dirname(Bun.main), "warm-module.js");

async function main()
{
    // A graph's first import runs Bun's module loader and some of Bun's own builtins for the first time; a graph of its
    // own does that first, outside the window.
    const warmGraph = new Bun.ModuleGraph({ globals: { pmbBase: 3 } });
    await warmGraph.import(pathToFileURL(warmPath).href);
    warmGraph.dispose();

    const host = require(cjsPath);
    let total = 0;
    for (let i = 0; i < 400; ++i)
        total = (total + host.pmbCount(i)) | 0;
    const lines = [`host ${total} ${host.calls()}`];

    const graph = new Bun.ModuleGraph({ globals: { pmbBase: 3 } });
    const before = t.statistics();
    const namespace = await graph.import(pathToFileURL(cjsPath).href);
    const after = t.statistics();
    // In the Consumer the load imports the wrapper's program and the wrapper function. The function is a child of a UCB
    // that carries the shared key whether it was imported or generated, so it imports either way, and one import would
    // pass even if the program had missed.
    t.expect("the graph's CommonJS module", t.delta(before, after), t.imports ? { sourceDigests: 0, imports: [">=", 2] } : { sourceDigests: 0 });
    const graphModule = namespace.default;
    t.check(graphModule.pmbCount !== host.pmbCount, "the graph shared the host's module");
    total = 0;
    for (let i = 0; i < 400; ++i)
        total = (total + graphModule.pmbCount(i)) | 0;
    lines.push(`graph ${total} ${graphModule.calls()}`);
    graph.dispose();
    console.log(lines.join("\n"));
    t.finish();
}

main().catch(error => {
    console.log(`failed: ${error}`);
    process.exitCode = 1;
});
