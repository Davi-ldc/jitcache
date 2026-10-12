// jitcache-host: bun
// jitcache-runs: Producer; ConsumerProducer; Consumer
// jitcache-expect-no-install: 1

// SPEC-ucb.md section 13.3, with-scope.js: THREAD Identity's with-scope bit. The jsc shell can set no global scope
// extension, so the extension comes from vm.compileFunction, which installs one on its context's global object and never
// removes it (F11). A program or indirect eval evaluated after it carries the with-scope bit only when the code that
// creates the executable making its request reads that extension (F18): globalFuncEval does for an indirect eval, and
// Interpreter::executeProgram for a program JSC::evaluate runs without a precompiled block, as Bun runs a CommonJS
// wrapper. vm.runInThisContext keeps the clear bit: NodeVMScript::unlinkedCodeBlockFor requests its UCB on a
// ProgramExecutable of its own, which nothing marks, and executeProgram runs that precompiled UCB without a request.
// - One text is evaluated by an indirect eval before and after that call. The Producer evaluates it at both points and
//   captures its body under the clear bit only, since the evaluation after meets the UCB the CodeCache kept; the
//   ConsumerProducer evaluates it after only, finds no body under the set bit (misses NoBody), generates it and
//   captures it under that bit. In the Consumer the text before imports, and after Bun.gc(true) has emptied the UCB's
//   sharing slot, the evaluation after meets that UCB through a CodeCache hit, whose SourceCodeKey ignores the bit:
//   the attach misses RequestKey, so no body is installed on a UCB whose record holds the other bit.
// - A CommonJS module required after the call, with Node's compile cache on, is decoded from a payload Bun generated
//   without the bit once the first run persisted it: the decoded root gets no record, its request misses RequestKey, and
//   none of its functions imports (misses NoKey) or is recorded, so none is captured.
const vm = require("node:vm");
const path = require("node:path");
const Module = require("node:module");
const { context } = require("./resources/ucb-bun.js");

const t = context();
Module.enableCompileCache(t.file("compile-cache"));

const text = "{ let withScopeTotal = 3; for (let i = 0; i < 3000; ++i) withScopeTotal = (withScopeTotal + i * 11) | 0; withScopeTotal; }";
const expected = 49483503;

if (t.role !== "ConsumerProducer") {
    const before = t.statistics();
    const value = (0, eval)(text);
    const after = t.statistics();
    t.check(value === expected, `the indirect eval gave ${value} under the clear bit`);
    if (t.role === "Consumer")
        t.expect("the indirect eval under the clear bit", t.delta(before, after), { imports: [">=", 1] });
}

// Empties the sharing slot of every live UCB; the CodeCache keeps the eval's.
Bun.gc(true);
vm.compileFunction("return 1;")();

{
    const before = t.statistics();
    const value = (0, eval)(text);
    const after = t.statistics();
    if (t.role === "Consumer")
        t.expect("the indirect eval under the set bit, through a CodeCache hit", t.delta(before, after), { attaches: 0, imports: 0, "misses.RequestKey": [">=", 1] });
    else if (t.role === "ConsumerProducer")
        t.expect("the indirect eval under the set bit, with no body under it", t.delta(before, after), { imports: 0, "misses.NoBody": [">=", 1], "records.Generated": [">=", 1] });
    console.log(`eval ${value}`);
}

{
    const before = t.statistics();
    const required = require(path.join(__dirname, "resources", "with-scope-module.js"));
    const loaded = t.statistics();
    let total = required.withScopeSum(10);
    const called = t.statistics();
    for (let i = 0; i < 400; ++i)
        total = (total + required.withScopeSum(i & 31)) | 0;
    if (t.role !== "Producer") {
        t.expect("the module decoded under the other bit", t.delta(before, loaded), { "misses.RequestKey": [">=", 1], imports: 0, seededDecodes: 0 });
        t.expect("the decoded module's functions", t.delta(loaded, called), {
            imports: 0,
            seededDecodes: 0,
            "misses.NoKey": [">=", 2],
            "records.Generated": 0,
            "records.Decoded": 0,
            "records.Imported": 0,
        });
    }
    console.log(`module ${total}`);
}
t.finish();
