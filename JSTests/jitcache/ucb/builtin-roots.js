// jitcache-host: bun

// SPEC-ucb.md section 13.3, builtin-roots.js, sections 3.5, 7.2.4, 7.2.6 and 7.3.6: builtin roots, whose identity is
// recorded at their creation and whose source digest is supplied rather than computed from their text.
// - JSC builtins. BuiltinExecutables keeps one UFE per builtin and VM, which the VM's first global object creates before
//   any script runs, so a later global object creates no builtin root while that cache holds it. Bun.shrink() empties
//   the cache (VM::deleteAllCode, which runs once the VM is idle), so the global object vm.createContext() creates next
//   makes each builtin it installs again, from generated BuiltinSourceMetadata: that creation computes no source digest
//   (sourceDigests unchanged) and takes one from the metadata for each root (suppliedSourceDigests grows). A builtin's
//   body imports at its first call in the Consumer (statistics): Array.prototype.findLastIndex's, the one import of its
//   window, since it calls only native functions and intrinsics besides a callback whose own first call comes before
//   the window.
// - Bun internal modules created from their text, as a `bun` run creates them: loading node:querystring records its
//   root once and takes its digest from its provider, the builtins section's table (form 2, statistics: a root, a
//   supplied digest, no digest computed from text), and its functions import at their first call in the Consumer.
// - Bun internal modules decoded by decodeBuiltinFunction, as a standalone executable decodes them: the app
//   resources/builtin-roots-app.js, built with `bun build --compile --bytecode`, makes the same checks of
//   node:querystring there. It runs twice (resources/ucb-bun.js, runApp). First it runs with this run's role over an
//   artifact of its own, where its decoded functions are captured and then seeded or imported at their first call. Then
//   it runs as a Consumer of this run's artifact, where its decoded functions must seed or import the bodies this
//   script's VM captured from the module it created. That happens only if the decoded root and its children have the
//   identities the created ones have.
// Each window makes its first calls through a function that runs once, so that no body of this script or of the app,
// which their Producers ran hot, imports or is seeded inside it: only the builtin's and the internal module's bodies
// can. The output, this script's and the app's, equals the JITCache-off run.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4); Bun calls delta at exit for every
// producing VM, the app's included.
const path = require("node:path");
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();

// The code deletion runs when this script's evaluation returns, before the timer.
Bun.shrink();
setTimeout(() => {
    const contexts = [];
    t.window("creating a global object once the builtin cache is empty", () => contexts.push(vm.createContext({ })), {
        sourceDigests: 0, suppliedSourceDigests: [">=", 1],
    });
    contexts.length = 0;

    const values = [3, 9, 4, 12, 7];
    const isLarge = value => value > 5;
    // isLarge's first call, which imports it in the Consumer, comes before the window.
    isLarge(values[0]);
    let found = t.window("Array.prototype.findLastIndex's first call", () => values.findLastIndex(isLarge), t.imports ? { imports: 1 } : { });
    for (let i = 0; i < 400; ++i)
        found = (found + values.findLastIndex(isLarge)) | 0;
    console.log(`findLastIndex ${found}`);

    const querystring = t.window("loading node:querystring", () => require("node:querystring"), t.configured ? {
        "counts.roots": [">=", 1], suppliedSourceDigests: [">=", 1], sourceDigests: 0,
    } : { });
    const inputs = ["a=1&b=two", "x=%20y&x=z", "plain", "k=v&k=w&q"];
    // The wrappers in `calls` run hot, so the windows call the module's functions through `firstCalls`, which run once.
    const calls = {
        escape: s => querystring.escape(s),
        parse: s => JSON.stringify(querystring.parse(s)),
    };
    const firstCalls = {
        escape: () => querystring.escape(inputs[0]),
        parse: () => JSON.stringify(querystring.parse(inputs[0])),
    };
    for (const name of ["escape", "parse"]) {
        const first = t.window(`querystring.${name}'s first call`, firstCalls[name], t.imports ? { imports: [">=", 1] } : { });
        let total = first.length;
        for (let i = 0; i < 400; ++i)
            total = (total + calls[name](inputs[i % inputs.length]).length) | 0;
        console.log(`querystring.${name} ${first} ${total}`);
    }

    const app = t.buildApp("builtin-roots-app", [path.join(__dirname, "resources", "builtin-roots-app.js")], { bytecode: true });
    t.runApp(app);
    t.runApp(app, { overRunArtifact: true });
    t.finish();
}, 0);
