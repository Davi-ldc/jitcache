// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// SPEC-cb.md section 11.3: CB state travels for every kind of body, not only for functions nested in a script. Each kind
// below runs hot enough to compile to baseline in the Producer and carries its profiles and counter to the Consumer:
// - program bodies: one source evaluated again and again through loadString, whose UCB the CodeCache serves to every
//   evaluation, so the UCB's LLInt counter crosses and a program CB compiles;
// - a module body, whose top level loops (resources/body-kinds-module.mjs), and the function it exports;
// - eval bodies: a direct eval at one call site and an indirect eval of one source;
// - a Function-constructor body;
// - builtin bodies: Array.prototype.reduce and Array.prototype.some.
// In twins builds the Consumer checks that each kind travelled. A body it can name by a function was imported at its
// first call (jitcacheBodyEvents). The program, eval and module bodies have no function to name, so it counts the
// imported bodies JITCache installed (jitcacheProgress) across the first run of each, when nothing else that runs is a
// body the Producer captured: one for the program's first evaluation, two for directEval's first call, which installs
// directEval and then its eval body, one for the first indirect eval and one for loading the module. A later evaluation
// of the same program or eval source makes a new CB of the same UCB, which takes the installed code natively.
(function main(role) {
    // In a twins Consumer, the number of imported bodies JITCache has installed so far; 0 in every other role and build,
    // where expectInstalls checks nothing.
    const countsInstalls = role === "Consumer" && typeof jitcacheProgress === "function";
    const installs = () => (countsInstalls ? jitcacheProgress().installs : 0);
    function expectInstalls(installed, expected, what) {
        if (countsInstalls && installed !== expected)
            throw new Error(`${what} installed ${installed} imported bodies, expected ${expected}`);
    }

    let total = 0;

    const programSource = "{ let sum = 0; for (let i = 0; i < 24; ++i) sum += i & 5; sum; }";
    let mark = installs();
    total += loadString(programSource);
    expectInstalls(installs() - mark, 1, "The program's first evaluation");
    for (let i = 1; i < 60; ++i)
        total += loadString(programSource);

    function directEval(x) {
        return eval("x * 2 + 1");
    }
    const indirectEval = eval;
    const indirectSource = "{ let sum = 0; for (let j = 0; j < 12; ++j) sum += j; sum; }";
    mark = installs();
    total += directEval(0);
    expectInstalls(installs() - mark, 2, "directEval's first call");
    mark = installs();
    total += indirectEval(indirectSource);
    expectInstalls(installs() - mark, 1, "The first indirect eval");
    for (let i = 1; i < 150; ++i) {
        total += directEval(i);
        total += indirectEval(indirectSource);
    }

    const adder = new Function("a", "b", "return a * 3 + b;");
    for (let i = 0; i < 300; ++i)
        total += adder(i, i & 3);

    const values = [1, 2, 3, 4, 5, 6];
    for (let i = 0; i < 300; ++i) {
        total += values.reduce((sum, value) => sum + value, i);
        total += values.some(value => value > (i & 7)) ? 1 : 0;
    }

    // The shell fetches the module file synchronously, so draining the microtasks finishes the import, and an assertion
    // below fails the run instead of rejecting a promise nobody reads.
    let module = null;
    let moduleError = null;
    mark = installs();
    import("./resources/body-kinds-module.mjs").then(namespace => module = namespace, error => moduleError = error);
    drainMicrotasks();
    if (!module)
        throw moduleError ?? new Error("the module did not finish loading");
    expectInstalls(installs() - mark, 1, "Loading the module");
    let moduleSum = module.moduleTotal;
    for (let i = 0; i < 300; ++i)
        moduleSum += module.moduleWeigh(values);
    print(total, moduleSum);

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(directEval, "call", "directEval");
        sections.expectImported(adder, "call", "the Function-constructor body");
        sections.expectImported(Array.prototype.reduce, "call", "Array.prototype.reduce");
        sections.expectImported(Array.prototype.some, "call", "Array.prototype.some");
        sections.expectImported(module.moduleWeigh, "call", "moduleWeigh");
    }
})(...arguments);
