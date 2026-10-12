// SPEC-ucb.md section 13.3, feedback-roundtrip.js, sections 5.1 to 5.5: the UCB feedback a body carries comes back as
// the producer left it, which T4 checks at every import (section 13.2), and the singleton state a body learned keeps
// the consumer's optimizing compiles from folding what the producer saw created twice (F14, F15).
//
// The functions live in a program evaluated in realms of its own (runString), so that one UCB has several CodeBlocks,
// one per realm, while the CodeCache shares the program's UCB:
// - frArith observes products, quotients, increments and overflows (arithmetic profiles); frArrays reads holes, indexes
//   past the end and typed arrays (array flags);
// - frExit reaches the DFG on int32 arguments and exits on strings until its optimized code is jettisoned, which adds
//   its exit site and clears its quick DFG bit;
// - frLoop enters the DFG from a loop, which sets didOptimize True;
// - frTier reaches the FTL in one realm, which sets both quick bits, while its CodeBlock in a second realm stays in the
//   baseline, where delta captures it;
// - frDead runs only in the LLInt in realms that die first, so a dying CodeBlock sets didOptimize False, and then
//   reaches the baseline in another realm;
// - frMakeAdder creates its closure frAdder, and with it the lexical environment frAdder reads, many times.
// A module written into the scratch directory under two names is one UCB whose module environment is created twice.
//
// The Producer shapes that state in extra realms, captures it with jitcacheDelta while they live (twins runs) and with
// delta at exit, and then runs the part every role runs. In the Consumer each first call imports (statistics), and
// twins runs read the imported bodies' feedback sections to confirm the states above travelled. Then a closure of
// frMakeAdder and a function of the module are each run until the DFG compiles them, a second closure or a second
// module environment is created, and their optimized code is not jettisoned (jettisons 0, jitcacheBodyEvents): the
// imported singleton bits kept the DFG from folding the callee and the scope, where the JITCache-off run folds and then
// jettisons. Every role prints the same results.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const hasEvents = typeof jitcacheBodyEvents === "function";

    const programText = `
        function frArith(a, b) { let x = a * b; x++; return (x + a / b) % 1000; }
        function frArrays(list, i) { const value = list[i]; return value === undefined ? -1 : value; }
        function frExit(x) { return x + 1; }
        function frLoop(n) { let total = 0; for (let i = 0; i < n; ++i) total = (total + i) | 0; return total; }
        function frTier(x) { return (x * 3 + 1) | 0; }
        function frDead(o) { return o.p + 1; }
        function frMakeAdder(k) { return function frAdder(x) { return x + k; }; }
    `;
    const moduleText = [
        "let frModuleBase = 7;",
        "export function frModuleRead(x) { return (x + frModuleBase) | 0; }",
        "let frModuleTotal = 0;",
        "for (let i = 0; i < 800; ++i) frModuleTotal = (frModuleTotal + i) | 0;",
        "export const frModuleResult = frModuleTotal;",
    ].join("\n");
    const moduleFiles = ["feedback-roundtrip-a.mjs", "feedback-roundtrip-b.mjs"].map(name => t.path(name));
    for (const file of moduleFiles)
        writeFile(file, moduleText);

    const arithArguments = [[3, 4], [7, 2], [0x7fffffff, 3], [2.5, 0.5], [9, -3], [1e10, 7]];
    const holes = [1, , 3];
    holes.length = 5;
    const arrayArguments = [[[1, 2, 3], 1], [holes, 1], [[4, 5], 9], [new Int8Array([6, 7]), 1], [{ 0: 8, length: 1 }, 0]];

    // Runs `call` until the DFG has compiled `fn` or `limit` calls have run; concurrent JIT is off in twins runs, so the
    // compile happens inside a call.
    function runUntilDFG(fn, call, limit)
    {
        for (let i = 0; i < limit && numberOfDFGCompiles(fn) < 1; ++i)
            call(i);
    }

    // The Producer's extra realms, which print nothing and leave nothing behind.
    function shapeCaptures()
    {
        // Realms whose frDead runs only in the LLInt and then dies with them. One CodeBlock dying while its UCB's
        // didOptimize is undetermined sets it False (CodeBlock::~CodeBlock).
        for (let attempt = 0; attempt < 3; ++attempt) {
            (function () {
                const doomed = runString(programText);
                for (let i = 0; i < 3; ++i)
                    doomed.frDead({ p: i });
            })();
            gc();
        }
        gc();

        const optimizing = runString(programText);
        const baseline = runString(programText);
        // The loops below call these functions directly, so the loops' own optimized code must not inline them.
        noInline(optimizing.frExit);
        noInline(optimizing.frLoop);
        noInline(optimizing.frTier);

        for (let i = 0; i < 100; ++i)
            optimizing.frExit(i);
        optimizeNextInvocation(optimizing.frExit);
        optimizing.frExit(1);
        for (let i = 0; i < 20000 && reoptimizationRetryCount(optimizing.frExit) < 1; ++i)
            optimizing.frExit(`s${i & 7}`);

        for (let i = 0; i < 4; ++i)
            optimizing.frLoop(10);
        optimizing.frLoop(400000);

        for (let i = 0; i < 400000; ++i) {
            optimizing.frTier(i);
            if (hasEvents && !(i & 1023) && jitcacheBodyEvents(optimizing.frTier, "call")?.ftlCompiles)
                break;
        }

        for (let i = 0; i < 600; ++i)
            optimizing.frMakeAdder(i)(i);

        // The second realm's CodeBlocks take the parked baseline code and stay in the baseline, where delta captures the
        // UCBs whose first realm's code is optimized.
        for (let i = 0; i < 20; ++i) {
            baseline.frLoop(4);
            baseline.frTier(i);
            baseline.frExit(i);
        }
        for (let i = 0; i < 60; ++i)
            baseline.frDead({ p: i });

        if (t.twins && t.produces)
            jitcacheDelta();
    }

    if (t.produces)
        shapeCaptures();

    const lines = [];
    function firstCall(label, call)
    {
        const before = t.statistics();
        const value = call();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), { imports: [">=", 1] });
        return value;
    }

    let realm = runString(programText);
    let total = firstCall("frArith's first call", () => realm.frArith(3, 4));
    for (let i = 0; i < 400; ++i) {
        const [a, b] = arithArguments[i % arithArguments.length];
        total = (total + realm.frArith(a, b)) % 100000;
    }
    lines.push(`arithmetic ${total}`);

    total = firstCall("frArrays's first call", () => realm.frArrays([1, 2], 0));
    for (let i = 0; i < 400; ++i) {
        const [list, index] = arrayArguments[i % arrayArguments.length];
        total += realm.frArrays(list, index);
    }
    lines.push(`arrays ${total}`);

    total = firstCall("frExit's first call", () => realm.frExit(1));
    for (let i = 0; i < 400; ++i)
        total = (total + realm.frExit(i)) | 0;
    lines.push(`exits ${total} ${realm.frExit("s")}`);

    total = firstCall("frLoop's first call", () => realm.frLoop(10));
    lines.push(`loop ${total} ${realm.frLoop(400000)}`);

    total = firstCall("frTier's first call", () => realm.frTier(1));
    for (let i = 0; i < 400; ++i)
        total = (total + realm.frTier(i)) | 0;
    lines.push(`tiers ${total}`);

    total = firstCall("frDead's first call", () => realm.frDead({ p: 1 }));
    for (let i = 0; i < 40; ++i)
        total += realm.frDead({ p: i });
    lines.push(`dead ${total}`);

    // What travelled, read from the bodies the Consumer imported.
    if (t.twins && t.imports) {
        const feedbackOf = fn => ucbSections.feedback(t.readSection(t.bodyKey(fn), "ucb.feedback"));
        const exit = feedbackOf(realm.frExit);
        t.check(exit.counts.exitSites >= 1, `frExit's body carries ${exit.counts.exitSites} exit sites`);
        t.check(exit.bytes[37] === 0, `frExit's quick DFG bit is ${exit.bytes[37]}`);
        t.check(feedbackOf(realm.frLoop).bytes[36] === 1, "frLoop's didOptimize is not True");
        const tier = feedbackOf(realm.frTier);
        t.check(tier.bytes[37] === 1 && tier.bytes[38] === 1, `frTier's quick bits are ${tier.bytes[37]} and ${tier.bytes[38]}`);
        t.check(feedbackOf(realm.frDead).bytes[36] === 0, "frDead's didOptimize is not False");
    }

    // A closure the producer saw created many times: its UFE's singleton bit and its scope's arrive set.
    const first = firstCall("frMakeAdder's first call", () => realm.frMakeAdder(3));
    // Both closures share one FunctionExecutable, which no caller may inline, so the DFG compiles frAdder itself.
    noInline(first);
    runUntilDFG(first, i => first(i), 100000);
    const second = realm.frMakeAdder(4);
    total = 0;
    for (let i = 0; i < 400; ++i)
        total = (total + first(i) + second(i)) | 0;
    lines.push(`adders ${total}`);
    if (hasEvents && t.imports) {
        const events = jitcacheBodyEvents(first, "call");
        t.check(events && events.jettisons === 0, () => `the first closure's optimized code was jettisoned: ${t.describe(events)}`);
    }
    realm = null;

    let moduleA;
    import(moduleFiles[0]).then(module => {
        moduleA = module;
        const before = t.statistics();
        let value = module.frModuleRead(1);
        const after = t.statistics();
        if (t.imports)
            t.expect("frModuleRead's first call", t.delta(before, after), { imports: [">=", 1] });
        noInline(module.frModuleRead);
        runUntilDFG(module.frModuleRead, i => { value = (value + module.frModuleRead(i)) | 0; }, 100000);
        lines.push(`module ${module.frModuleResult}`);
        return import(moduleFiles[1]);
    }).then(moduleB => {
        let value = 0;
        for (let i = 0; i < 400; ++i)
            value = (value + moduleA.frModuleRead(i) + moduleB.frModuleRead(i)) | 0;
        lines.push(`modules ${moduleB.frModuleResult} ${value}`);
        if (hasEvents && t.imports) {
            const events = jitcacheBodyEvents(moduleA.frModuleRead, "call");
            t.check(events && events.jettisons === 0, () => `the first module's function was jettisoned: ${t.describe(events)}`);
        }
        moduleA = null;
        for (const line of lines)
            print(line);
        t.finish();
    }).catch(error => {
        print(`failed: ${error}`);
        throw error;
    });
})(...arguments);
