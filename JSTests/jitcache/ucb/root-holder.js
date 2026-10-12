// SPEC-ucb.md section 13.3, root-holder.js, F15 and section 5.1: a root UFE's own singleton bit,
// m_singletonHasBeenInvalidated, stays local, so in the Consumer it is false until a second function of one executable
// exists, as in the JITCache-off run. The Producer creates each root's functions and runs them into the optimizing tiers.
// In the Consumer each root's body imports at the root's first call (statistics: an import, and the holder digest the
// root's context covers), and the results equal the JITCache-off run.
//
// Function-constructor roots. Two calls of a Function constructor with one text, with no other such call between them,
// give one FunctionExecutable: FunctionExecutable::fromGlobalCode returns the realm's last one
// (JSGlobalObject::tryGetCachedFunctionExecutableForFunctionConstructor). The second JSFunction then invalidates the
// executable's singleton and sets the root UFE's bit (FunctionExecutable::notifyCreation).
// - A `new Function` text, created twice that way. Its body never reads its callee slot, because the constructor's
//   function is a declaration, whose name does not alias the callee, so no compile of it depends on the singleton.
// - The same with the generator form of the Function constructor, whose wrapper reads its callee at create_generator.
//   Its DFG compile folds the callee through the singleton while the singleton holds, and watches it
//   (ByteCodeParser::get, handleCreateInternalFieldObject). Once that compile exists, creating the second function
//   jettisons it exactly once, which happens only while the bit is still false. Had the bit travelled from the Producer,
//   which set it, link would have started the executable invalidated and nothing would be jettisoned. The jettison is
//   native behavior, so every role of a twins build asserts it through the body's event counts
//   (SPEC-integrator.harness.md section 10.2).
//
// A builtin root. Array.prototype.findLast is created in this realm and in a second one, from the one UFE
// BuiltinExecutables keeps per VM. Each realm links a FunctionExecutable of its own and creates one function from it, so
// no run creates a second function of one executable for a builtin (report/spec/ucb.11/root-holder-second-function.md).
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const rounds = 3000;
    // Enough calls to reach the DFG and few enough to stay out of the FTL, whose compile would watch the singleton too.
    const generatorRounds = 400;

    // A root's first call imports its bodies, each with its context and so the root UFE's holder digest. The window counts
    // the root's bodies alone: what the call also requests for the first time, a callback, drain and the generator
    // builtins it runs, is requested once before the window.
    function firstCall(label, bodies, call)
    {
        const before = t.statistics();
        const value = call();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), { imports: bodies, holderDigests: [">=", 1] });
        return value;
    }

    const text = "let total = n; for (let i = 0; i < 8; ++i) total = (total * 3 + i) | 0; return total;";
    const firstFunction = new Function("n", text);
    const secondFunction = new Function("n", text);
    let constructedTotal = firstCall("the new Function body's first call", 1, () => firstFunction(2));
    for (let i = 0; i < rounds; ++i)
        constructedTotal = (constructedTotal + firstFunction(i & 3) + secondFunction(i & 7)) | 0;

    const GeneratorFunction = Object.getPrototypeOf(function* () { }).constructor;
    const generatorText = "let total = n; for (let i = 0; i < 4; ++i) total = (total * 5 + i) | 0; yield total; yield (total ^ n) | 0;";
    function drain(generator)
    {
        let sum = 0;
        for (const value of generator)
            sum = (sum + value) | 0;
        return sum;
    }
    function* warmGenerator()
    {
        yield 1;
    }
    drain(warmGenerator());
    const firstGenerator = new GeneratorFunction("n", generatorText);
    // The wrapper compiles on its own rather than inlined into this function's optimized code, so its own DFG compile
    // is the one that watches the singleton.
    noInline(firstGenerator);
    // The wrapper's body and the generator body nested in it.
    let generatedTotal = firstCall("the generator Function body's first call", 2, () => drain(firstGenerator(2)));
    for (let i = 0; i < generatorRounds; ++i)
        generatedTotal = (generatedTotal + drain(firstGenerator(i & 3))) | 0;
    // Results stay out of this loop, whose length depends on when the DFG compiles.
    for (let i = 0; i < 20000 && numberOfDFGCompiles(firstGenerator) < 1; ++i)
        drain(firstGenerator(i & 3));
    const before = t.events(firstGenerator);
    const secondGenerator = new GeneratorFunction("n", generatorText);
    const after = t.events(firstGenerator);
    if (before) {
        t.check(numberOfDFGCompiles(firstGenerator) >= 1, "the generator Function's wrapper never reached the DFG");
        const jettisons = after.jettisons - before.jettisons;
        t.check(jettisons === 1, () => `creating the second generator function jettisoned ${jettisons} compiles of the wrapper, expected 1`);
    }
    for (let i = 0; i < generatorRounds; ++i)
        generatedTotal = (generatedTotal + drain(secondGenerator(i & 7))) | 0;

    const values = [3, 8, 5, 12, 7, 2];
    const isLarge = value => value > 6;
    isLarge(0);
    let found = firstCall("Array.prototype.findLast's first call", 1, () => values.findLast(isLarge));
    for (let i = 0; i < rounds; ++i)
        found = (found + values.findLast(isLarge)) | 0;
    let otherRealm = runString("");
    const otherFindLast = otherRealm.Array.prototype.findLast;
    otherRealm = null;
    for (let i = 0; i < rounds; ++i)
        found = (found + otherFindLast.call(values, isLarge)) | 0;

    print(`roots ${constructedTotal} ${generatedTotal} ${found}`);
    t.finish();
})(...arguments);
