// The app of supplied-digests.js, built with `bun build --compile` together with supplied-digests-module.js, which it loads
// at run time. supplied-digests.js runs it as a JITCache VM of its own with the run's role (resources/ucb-bun.js, runApp);
// the sequence index, which runApp passes on, picks the case: 0 and 1, built with --bytecode, with strict on and then off
// in the Consumer; 2, built without it; 3, built without it and with the digest record removed; 4 and 5, built as
// CommonJS without it, with the module wrapper overridden in the Producer or in the Consumer before the module loads. The
// checks are SPEC-ucb.md section 13.3's, listed in supplied-digests.js.
//
// Bun creates some of its own JS builtins while it creates the global object, after JITCache's start, and their roots are
// hashed from their text: each builtin's source is a range of one provider that holds them all, so none spans its
// provider (section 3.5, form 3). The counts at the app's first statement therefore do not single out the entry module,
// and the digest checks are made over the windows below, around the loads they name.
const path = require("node:path");
const Module = require("node:module");
const { appContext } = require("./ucb-bun.js");

const t = appContext();
const decodes = t.sequence <= 1;
const strict = t.sequence !== 1;
const hasRecord = t.sequence !== 3;
const overrides = (t.sequence === 4 && t.role === "Producer") || (t.sequence === 5 && t.role !== "Producer");

// At the app's first statistics read, the entry module has been decoded or imported. Without --bytecode its body was
// captured, so the Consumer imported it, and with strict on that import verified the digest its key rests on.
const atStart = t.statistics();
if (atStart && t.imports && !decodes) {
    t.check(atStart.imports >= 1, "the entry module did not import");
    const verified = hasRecord && strict ? atStart.suppliedDigestVerifications >= 1 : atStart.suppliedDigestVerifications === 0;
    t.check(verified, () => `the entry module's import made ${atStart.suppliedDigestVerifications} verifications`);
}

// What a function's first call in the Consumer does: a decoded function is seeded, a generated one imports.
const firstCall = decodes ? { seededDecodes: [">=", 1], suppliedDigestVerifications: 0 } : { imports: [">=", 1], suppliedDigestVerifications: 0 };

// The overriding wrapper. JSCommonJSModule::evaluate keeps a module's text from its first newline, which in a CommonJS
// build ends the `// @bun @bun-cjs` comment, and drops its last four characters, which hold the `})` closing the bundled
// wrapper. The text left opens the bundled wrapper and never closes it, so this wrapper closes and calls it, and the
// module runs under other text with its own exports, in the JITCache-off run as in every other.
const wrapperStart = "(function(exports,require,module,__filename,__dirname){return(";
const wrapperEnd = "}).call(this,exports,require,module,__filename,__dirname))})";

// Where the module's digest comes from: decoded from bytecode or loaded with its record, the executable's record; under an
// overridden wrapper, which clears the digest the record gives its provider, and without a record, its own text.
let loadExpectations = { };
if (t.configured && decodes)
    loadExpectations = { sourceDigests: 0, suppliedSourceDigests: [">=", 2] };
else if (t.configured && overrides)
    loadExpectations = { sourceDigests: 1 };
else if (t.configured && t.sequence >= 4)
    loadExpectations = { sourceDigests: 0, suppliedSourceDigests: [">=", 1] };
else if (t.configured)
    loadExpectations = { suppliedSourceDigests: [">=", 1] };
const loaded = t.window("loading node:querystring and the embedded module", () => {
    const querystring = require("node:querystring");
    // Bun applies Module.wrapper when it evaluates a CommonJS module; nothing in its loader calls Module.wrap.
    if (overrides)
        Module.wrapper = [wrapperStart, wrapperEnd];
    return { querystring, module: require(path.join(__dirname, "supplied-digests-module.js")) };
}, loadExpectations);

function sdHot(x)
{
    let total = x;
    for (let i = 0; i < 4; ++i)
        total = (total * 7 + i) | 0;
    return total;
}

// A direct eval of one text, which reads `n` from the decoded function's scope, so every call evaluates the same body.
function sdEval(n)
{
    return eval("{ let sdEvalTotal = n; for (let i = 0; i < 300; ++i) sdEvalTotal = (sdEvalTotal + i) | 0; sdEvalTotal; }");
}

// An ES5 constructor: the bytecode generated ahead of time holds only its call body (F2), so `new` requests its
// construct body, which imports after verifying the module's supplied digest once, with strict on.
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

// With --bytecode and strict on, nothing has imported yet a body whose key rests on the entry module's digest, so the
// direct eval's import is the first that could, and verifies nothing (section 3.5): its key ends in its own text's digest.
total = t.window("sdEval's first call", () => sdEval(1), t.imports ? { suppliedDigestVerifications: 0, imports: [">=", 1] } : { });
for (let i = 0; i < 40; ++i)
    total = (total + sdEval(i)) | 0;
lines.push(`sdEval ${total}`);

const pointExpectations = !t.imports ? { } : decodes && strict ? { imports: 1, suppliedDigestVerifications: 1 } : { imports: 1, suppliedDigestVerifications: 0 };
total = t.window("new SdPoint()'s first construction", () => new SdPoint(2).x, pointExpectations);
for (let i = 0; i < 400; ++i)
    total = (total + new SdPoint(i).scaled(3)) | 0;
lines.push(`SdPoint ${total}`);

// The embedded module's root never reaches the baseline, so it is never imported: without --bytecode, its function's
// import is the first to rest on its recorded digest, and verifies it, with strict on. Under the other run's wrapper the
// function's key has no body at all.
let moduleExpectations = { };
if (t.imports && t.sequence >= 4)
    moduleExpectations = { imports: 0, seededDecodes: 0, "misses.NoBody": [">=", 1] };
else if (t.imports && decodes)
    moduleExpectations = firstCall;
else if (t.imports)
    moduleExpectations = { imports: [">=", 1], suppliedDigestVerifications: hasRecord ? 1 : 0 };
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
