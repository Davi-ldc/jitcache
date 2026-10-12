// jitcache-host: bun

// SPEC-ucb.md section 13.3, drop-and-reimport.js, THREAD Restoration: a body whose UCB Bun drops on Bun.gc(true) goes
// through its request point again at its next request. Bun.gc(true) runs Heap::deleteAllUnlinkedCodeBlocks, which
// clears the code of every UFE in the clearable set, and then a full collection (JSC__VM__runGC). The function below is
// the completion value of a program that vm.runInThisContext evaluates; the CodeCache keeps the program's UCB, so each
// evaluation's function is a new FunctionExecutable of the one child UFE, whose slot Bun.gc(true) empties.
// - In the Consumer the function's first call imports (statistics), and after Bun.gc(true) and the death of the
//   evaluation that held it, the next evaluation's first call imports again.
// - The registry's counts drop with the UCBs that die (statistics counts.codeBlocks), and the registry holds no entry for
//   a dead cell ($vm's verifyRegistry, through jitcacheUCBStatistics({ verifyRegistry: true })).
// The runs keep the Image lane's twin check, whose twin CodeBlock keeps nothing alive past it (SPEC-image.md I20).
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4); Bun calls delta at exit for the
// Producer.
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const text = "(function dropAndReimport(x) { let total = x; for (let i = 0; i < 4; ++i) total = (total * 9 + i) | 0; return total; })";
const rounds = 400;

// Evaluates the program, calls its function `rounds` times and returns the sum; the evaluation's function dies with
// this call's frame.
function evaluateAndRun(label)
{
    const fn = vm.runInThisContext(text);
    const before = t.statistics();
    let total = fn(1);
    const after = t.statistics();
    if (t.imports)
        t.expect(label, t.delta(before, after), { imports: 1 });
    for (let i = 0; i < rounds; ++i)
        total = (total + fn(i)) | 0;
    return total;
}

const first = evaluateAndRun("the first evaluation's first call");
const countsBefore = t.statistics()?.counts;
Bun.gc(true);
Bun.gc(true);
const countsAfter = t.statistics()?.counts;
if (countsBefore && countsAfter)
    t.check(countsAfter.codeBlocks < countsBefore.codeBlocks, () => `the registry still counts ${countsAfter.codeBlocks} UCBs after Bun.gc(true), as many as the ${countsBefore.codeBlocks} before`);
const second = evaluateAndRun("the first call after Bun.gc(true)");
console.log(`totals ${first} ${second}`);
t.finish();
