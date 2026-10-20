// The app of decoded-code-bun.js, built with `bun build --compile --bytecode` together with decoded-code-bun-module.js and
// warm-module.js, which it requires at run time from beside it in the executable, and run as a JITCache VM of its own
// with the run's role (resources/ucb-bun.js, runApp). Bun decodes the embedded modules from the bytecode the
// executable carries, and node:util with every internal module it can require from the internal modules' bytecode
// beside it (decodeBuiltinFunction). In the last sequence the app is built without --bytecode, and its bodies import.
//
// In the decoding builds:
// - no decoded module, function or internal module gets a record or imports or attaches across its load and calls
//   (statistics), and `new DcbPoint`, whose call body alone the executable caches, generates its construct body, which
//   misses NoKey in a run that imports;
// - $vm.returnCodeToBytecodeCacheWhenIdle() (BUN_JSC_useDollarVM=1) returns the decoded functions' code to the payload
//   once the VM is idle, which dcbCounter's finished generator shows, and the module's second evaluation decodes them
//   again, still with no record.
// The app's output, which decoded-code-bun.js prints, equals the JITCache-off run.
const path = require("node:path");
const { appContext } = require("./ucb-bun.js");

const t = appContext();
const decodes = t.sequence <= 2;
// The embedded modules sit beside the entry module in the executable, whose path is Bun.main. A bundle inlines
// `__dirname` as the directory the app was built from, so path.join(__dirname, ...) would load a source file from disk.
const modulePath = path.join(path.dirname(Bun.main), "decoded-code-bun-module.js");
require(path.join(path.dirname(Bun.main), "warm-module.js"));
// JSC's builtins are roots of their own, which get records at their first run, so the ones the windows' code reaches
// run here first: a generator's resumption, and Function.prototype.call, which node:util's primordials call through a
// bound function at load (a direct `f.call(...)` never runs the builtin's body).
(function* warmGenerator() { yield 0; })().next();
Function.prototype.call.bind(Math.max)(null, 1, 2);

// What each window allows: in a decoding build, no record, import or attach; in the other build, an import in a run
// that imports, for the bodies its Producer ran hot.
const nothing = { "records.Generated": 0, "records.Imported": 0, imports: 0, attaches: 0 };
const firstCall = decodes ? nothing : t.imports ? { imports: [">=", 1] } : { };
const lines = [];

const mod = t.window("loading the embedded module", () => require(modulePath), decodes ? nothing : { });
let total = t.window("dcbSum's first call", () => mod.dcbSum(1), firstCall);
for (let i = 0; i < 400; ++i)
    total = (total + mod.dcbSum(i)) | 0;
lines.push(`dcbSum ${total}`);

const construction = decodes ? { ...nothing, "misses.NoKey": t.imports ? [">=", 1] : 0 } : firstCall;
total = t.window("DcbPoint's first construction", () => new mod.DcbPoint(2).x, construction);
for (let i = 0; i < 400; ++i)
    total = (total + mod.DcbPoint(i) + new mod.DcbPoint(i).x) | 0;
lines.push(`DcbPoint ${total}`);

const generator = t.window("dcbCounter's first resumption", () => {
    const created = mod.dcbCounter(3);
    created.next();
    return created;
}, decodes ? nothing : { });
// Heap::deleteAllUnlinkedCodeBlocks keeps the code a suspended generator resumes in, so the generator runs to its end
// before the code goes back.
let counted = 0;
for (let step = generator.next(); !step.done; step = generator.next())
    counted += step.value;
lines.push(`dcbCounter ${counted}`);

const util = t.window("loading node:util", () => require("node:util"), decodes ? nothing : { });
let formatted = t.window("util.format's first call", () => util.format("%s=%d", "dcb", 1), firstCall);
for (let i = 0; i < 400; ++i)
    formatted = util.format("%s=%d", "dcb", i);
lines.push(`format ${formatted}`);

$vm.returnCodeToBytecodeCacheWhenIdle();
setTimeout(() => {
    // The decoded code went back to its payload once the VM was idle.
    if (decodes)
        t.check($vm.isGeneratorBodyCodeInBytecodeCache(generator) === true, "dcbCounter's body was not returned to its payload");

    // The module's second evaluation makes new functions of the same code, whose first calls decode it again.
    delete require.cache[require.resolve(modulePath)];
    const again = t.window("the module's second evaluation", () => require(modulePath), decodes ? nothing : { });
    t.check(again.dcbSum !== mod.dcbSum, "requiring the module again did not evaluate it again");
    total = t.window("dcbSum's first call in the second evaluation", () => again.dcbSum(4), decodes ? nothing : { });
    total = (total + t.window("DcbPoint's first construction in the second evaluation", () => new again.DcbPoint(5).x, decodes ? nothing : { })) | 0;
    lines.push(`again ${total}`);
    console.log(lines.join("\n"));
    t.finish();
}, 0);
