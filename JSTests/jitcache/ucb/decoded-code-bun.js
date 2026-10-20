// jitcache-host: bun
// jitcache-runs: Off; Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Off; Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-delta-at-exit
// jitcache-runs: Off; Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 0:2
// jitcache-expect-no-install: 1:2
// jitcache-expect-no-install: 2:2

// decoded-code.js for Bun: code Bun decodes from a bytecode payload gets no record and no identity, so nothing in it
// imports or is captured. Two payloads:
// - Node's compile cache. This script loads resources/decoded-code-bun-module.js with module.enableCompileCache(), and
//   the first run of the first three sequences, with JITCache off, writes the cache, so every later run decodes the
//   same payload. Across the module's load and its functions' first calls and first construction, no record is made and
//   nothing imports or attaches (statistics). In sequence 2 the Producer decodes and its delta at exit commits nothing
//   for the decoded module, so the Consumer, which loads the same text without the cache, misses NoBody at the module
//   and at each function.
// - A `bun build --compile --bytecode` executable, the app (resources/decoded-code-bun-app.js), whose entry module calls
//   functions of a module beside it and requires node:util, so that internal modules decode too. It runs as a JITCache
//   VM of its own with this run's role, and with delta at exit in a producing run (resources/ucb-bun.js, runApp), and
//   makes its own checks, among them the functions $vm.returnCodeToBytecodeCacheWhenIdle() returns to the payload,
//   which decode again with no record.
// Sequence 3 runs the same module without the cache and the app built without --bytecode: the Consumer imports their
// bodies (statistics). The consumers of decoded code may install nothing of it. Every run, the app's output included,
// equals the JITCache-off run. The first run tells itself from the oracle's Off runs by a file it leaves in the scratch
// directory.
const fs = require("node:fs");
const path = require("node:path");
const Module = require("node:module");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const cached = t.sequence <= 2;
// The runs that load the module from the compile cache; the Consumer of sequence 2 and both runs of sequence 3 do not.
const decodes = cached && !(t.sequence === 2 && t.role === "Consumer");
const cacheDirectory = t.file("decoded-code-bun-compile-cache");
const marker = t.file("decoded-code-bun.cache-written");
const modulePath = path.join(__dirname, "resources", "decoded-code-bun-module.js");
const warmPath = path.join(__dirname, "resources", "warm-module.js");

if (t.off && cached && !fs.existsSync(marker)) {
    Module.enableCompileCache(cacheDirectory);
    require(warmPath);
    require(modulePath);
    Module.flushCompileCache();
    fs.writeFileSync(marker, "the first run wrote the compile cache");
    console.log("the first run wrote the compile cache");
    process.exit(0);
}
if (decodes)
    Module.enableCompileCache(cacheDirectory);
// The loader's first run of the path the module takes, outside the windows.
require(warmPath);

// What each window allows, by the kind of run.
const nothing = { "records.Generated": 0, "records.Imported": 0, imports: 0, attaches: 0 };
let expectations = { };
if (decodes)
    expectations = nothing;
else if (t.imports && cached)
    expectations = { imports: 0, "misses.NoBody": [">=", 1] };
else if (t.imports)
    expectations = { imports: [">=", 1] };

// Statistics are read around the calls themselves: this helper's own request comes before its first read, while an
// action passed as a closure would make a request of its own inside the window.
function observe(label, fn, x, construct)
{
    const before = t.statistics();
    const value = construct ? new fn(x) : fn(x);
    const after = t.statistics();
    t.expect(label, t.delta(before, after), expectations);
    return value;
}

const mod = observe("loading the module", require, modulePath, false);
let total = observe("dcbSum's first call", mod.dcbSum, 1, false);
total = (total + observe("DcbPoint's first construction", mod.DcbPoint, 2, true).x) | 0;
for (let i = 0; i < 400; ++i)
    total = (total + mod.dcbSum(i) + mod.DcbPoint(i) + new mod.DcbPoint(i).x) | 0;
console.log(`module ${total}`);

const entries = ["decoded-code-bun-app.js", "decoded-code-bun-module.js", "warm-module.js"].map(name => path.join(__dirname, "resources", name));
const app = cached ? t.buildApp("decoded-code-bun-bytecode-app", entries, { bytecode: true }) : t.buildApp("decoded-code-bun-app", entries, { format: "cjs" });
t.runApp(app, { flags: t.produces ? ["--jitcache-delta-at-exit"] : [], environment: { BUN_JSC_useDollarVM: "1" } });
t.finish();
