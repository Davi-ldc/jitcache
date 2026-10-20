// The app of supplied-digests.js, built with `bun build --compile` together with supplied-digests-module.js and
// warm-module.js, which it loads at run time. supplied-digests.js runs it as a JITCache VM of its own with the run's role and strict on
// (resources/ucb-bun.js, runApp); the sequence index, which runApp passes on, picks the case: 0, an ES build; 1, the same
// with the digest record removed; 2 and 3, built as CommonJS, with the module wrapper overridden in the Producer or in the
// Consumer before the module loads. The checks are listed in supplied-digests.js.
//
// Bun creates some of its own JS builtins while it creates the global object, after JITCache's start, and their roots are
// hashed from their text: each builtin's source is a range of one provider that holds them all, so none spans its
// provider, and no provider supplies its digest. The counts of computed and supplied digests at the app's first
// statement therefore do not single out the entry module, and those checks are made over the windows below, around the
// loads they name.

// The statistics at the entry module's first statement, read before the app loads any other module. A Consumer that
// imports a function of an internal module verifies that module's own supplied digest with strict on, since its
// provider supplies it, so after the requires below the verification count would no longer single out the entry
// module's import.
// bun:jsc is native, so loading it runs no internal module's code.
const atFirstStatement = require("bun:jsc").jitcacheUCBStatistics?.();
const path = require("node:path");
const Module = require("node:module");
const { appContext } = require("./ucb-bun.js");

const t = appContext();
const hasRecord = t.sequence !== 1;
const overrides = (t.sequence === 2 && t.role === "Producer") || (t.sequence === 3 && t.role !== "Producer");
// 0 and 1 are ES builds, where the bundler makes the CommonJS embedded module an ES module whose default export is its
// `module.exports` (`export default require_...()`), and require of an ES module returns its namespace.
const esBuild = t.sequence <= 1;

// At its first statement, the entry module has been imported: its body was captured, so the Consumer imported it, and
// with strict on that import verified the digest its key rests on. In an ES build, Bun's provider of the module looked
// up its analysis when Bun created it, which hashed the text once before: with the record, to check the recorded
// digest, a verification of its own; without it, to compute the digest the provider then supplies, which the import
// verifies as it verifies a recorded one.
const atStart = t.observes ? atFirstStatement : null;
if (atStart && t.imports) {
    t.check(atStart.imports >= 1, "the entry module did not import");
    const verifications = esBuild && hasRecord ? 2 : 1;
    t.check(atStart.suppliedDigestVerifications === verifications, () => `the entry module's import made ${atStart.suppliedDigestVerifications} verifications, expected ${verifications}`);
}

// What a function's first call in the Consumer does: it imports, and verifies nothing more.
const firstCall = { imports: [">=", 1], suppliedDigestVerifications: 0 };

// The overriding wrapper. JSCommonJSModule::evaluate keeps a module's text from its first newline, which in a CommonJS
// build ends the `// @bun @bun-cjs` comment, and drops its last four characters, which hold the `})` closing the bundled
// wrapper. The text left opens the bundled wrapper and never closes it, so this wrapper closes and calls it, and the
// module runs under other text with its own exports, in the JITCache-off run as in every other.
const wrapperStart = "(function(exports,require,module,__filename,__dirname){return(";
const wrapperEnd = "}).call(this,exports,require,module,__filename,__dirname))})";

// Where the module's digest comes from: loaded with its record, the executable's record; under an overridden wrapper,
// which clears the digest the record gives its provider, and without a record, its own text. node:querystring's comes
// from the internal modules' digest table.
let loadExpectations = { };
if (t.configured && overrides)
    loadExpectations = { sourceDigests: 1 };
else if (t.configured && t.sequence >= 2)
    loadExpectations = { sourceDigests: 0, suppliedSourceDigests: [">=", 1] };
else if (t.configured && hasRecord)
    loadExpectations = { sourceDigests: 0, suppliedSourceDigests: [">=", 2] };
else if (t.configured)
    loadExpectations = { sourceDigests: [">=", 1], suppliedSourceDigests: [">=", 1] };
// The embedded module sits beside the entry module in the executable, whose path is Bun.main. A bundle inlines
// `__dirname` as the directory the app was built from, so path.join(__dirname, ...) would load the module's source file
// from disk, without its recorded digest.
const embeddedModule = path.join(path.dirname(Bun.main), "supplied-digests-module.js");
// The first runtime require of an embedded module runs some of Bun's own builtins for the first time, whose roots Bun
// hashes from their text; it happens here, outside the window.
require(path.join(path.dirname(Bun.main), "warm-module.js"));
const loaded = t.window("loading node:querystring and the embedded module", () => {
    const querystring = require("node:querystring");
    // Bun applies Module.wrapper when it evaluates a CommonJS module; nothing in its loader calls Module.wrap.
    if (overrides)
        Module.wrapper = [wrapperStart, wrapperEnd];
    const embedded = require(embeddedModule);
    return { querystring, module: esBuild ? embedded.default : embedded };
}, loadExpectations);

function sdHot(x)
{
    let total = x;
    for (let i = 0; i < 4; ++i)
        total = (total * 7 + i) | 0;
    return total;
}

// A direct eval of one text, which reads `n` from the imported function's scope, so every call evaluates the same body.
function sdEval(n)
{
    return eval("{ let sdEvalTotal = n; for (let i = 0; i < 300; ++i) sdEvalTotal = (sdEvalTotal + i) | 0; sdEvalTotal; }");
}

// An ES5 constructor: `new` requests its construct body, which imports with no further verification, since the entry
// module's import verified the digest its key rests on.
function SdPoint(x)
{
    this.x = x;
}
SdPoint.prototype.scaled = function scaled(k)
{
    return this.x * k;
};

const lines = [];
let total = t.window("sdHot's first call", () => sdHot(1), t.imports ? firstCall : { });
for (let i = 0; i < 400; ++i)
    total = (total + sdHot(i)) | 0;
lines.push(`sdHot ${total}`);

// The direct eval's key ends in its own text's digest, so its import verifies nothing.
total = t.window("sdEval's first call", () => sdEval(1), t.imports ? { suppliedDigestVerifications: 0, imports: [">=", 1] } : { });
for (let i = 0; i < 40; ++i)
    total = (total + sdEval(i)) | 0;
lines.push(`sdEval ${total}`);

const pointExpectations = t.imports ? { imports: 1, suppliedDigestVerifications: 0 } : { };
total = t.window("new SdPoint()'s first construction", () => new SdPoint(2).x, pointExpectations);
for (let i = 0; i < 400; ++i)
    total = (total + new SdPoint(i).scaled(3)) | 0;
lines.push(`SdPoint ${total}`);

// The embedded module's root never reaches the baseline, so it is never imported: its function's import is the first to
// rest on its provider's digest, the recorded one or, without the record, the one its analysis lookup computed, and
// verifies it, with strict on. Under the other run's wrapper the function's key has no body at all.
let moduleExpectations = { };
if (t.imports && t.sequence >= 2)
    moduleExpectations = { imports: 0, "misses.NoBody": [">=", 1] };
else if (t.imports)
    moduleExpectations = { imports: [">=", 1], suppliedDigestVerifications: 1 };
total = t.window("the embedded module's function's first call", () => loaded.module.sdModuleHot(1), moduleExpectations);
for (let i = 0; i < 400; ++i)
    total = (total + loaded.module.sdModuleHot(i)) | 0;
lines.push(`module ${total} ${loaded.querystring.escape("a b")}`);

if (t.twins)
    t.check(t.status() === null, () => `cache activity turned off at ${t.status()}`);

// The entry module's own body reaches the baseline here, so the Producer captures it and later runs import it.
let moduleTotal = 0;
for (let i = 0; i < 3000; ++i)
    moduleTotal = (moduleTotal + i * 3) | 0;
lines.push(`entry ${moduleTotal}`);
console.log(lines.join("\n"));
t.finish();
