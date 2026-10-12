// SPEC-ucb.md section 13.3, parse-fields.js, F3: an imported function body restores its UFE's parse fields (section 7.4)
// before newCodeBlockFor copies them to the FunctionExecutable. A sloppy function with a non-simple parameter list gets an
// unmapped arguments object whose callee accessor throws, which it does only when the executable has the
// non-simple-parameter feature; f.caller and the arguments object behave as in the JITCache-off run for strict and
// sloppy imported functions. Every body below imports in the Consumer (statistics, imports).
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);

    function nonSimpleCallee(a = 0)
    {
        return arguments.callee;
    }
    function sloppyCaller()
    {
        return sloppyCaller.caller ? `called from ${sloppyCaller.caller.name}` : "no caller";
    }
    function sloppyMapped(a)
    {
        arguments[0] = 7;
        return a;
    }
    function nonSimpleUnmapped(a = 1)
    {
        arguments[0] = 7;
        return a;
    }
    function strictCaller()
    {
        "use strict";
        return strictCaller.caller;
    }
    function strictUnmapped(a)
    {
        "use strict";
        arguments[0] = 7;
        return a;
    }
    function strictCallee()
    {
        "use strict";
        return arguments.callee;
    }

    function outcome(fn, argument)
    {
        try {
            const value = fn(argument);
            return typeof value === "function" ? `function ${value.name}` : String(value);
        } catch (error) {
            return `${error.name}: ${error.message}`;
        }
    }

    // outcome's own first request, which imports in the Consumer, stays out of the windows below.
    outcome(() => 0, 0);
    for (const fn of [nonSimpleCallee, sloppyCaller, sloppyMapped, nonSimpleUnmapped, strictCaller, strictUnmapped, strictCallee]) {
        const before = t.statistics();
        const first = outcome(fn, 3);
        const after = t.statistics();
        if (t.imports)
            t.expect(`${fn.name}'s first call`, t.delta(before, after), { imports: [">=", 1] });
        for (let i = 0; i < 400; ++i) {
            const again = outcome(fn, i);
            if (fn !== sloppyMapped && fn !== nonSimpleUnmapped && fn !== strictUnmapped && again !== first)
                throw new Error(`${fn.name} gave ${again} after ${first}`);
        }
        print(`${fn.name}: ${first}`);
    }
    t.finish();
})(...arguments);
