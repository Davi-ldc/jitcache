// jitcache-pin: off the derived default constructor runs until the DFG compiles it, and what the DFG freezes from its super_construct cache is what the native fix of SPEC-ics.md E1 changes

// SPEC-ucb.md section 13.3, request-kinds.js: a body of each kind of request point imports in the Consumer (statistics,
// imports) and passes its twins (SPEC-ucb.md section 13.2, which the engine runs at each import of a twins run): a
// program, an indirect eval, a direct eval nested in a function, a Function-constructor body, a JSC builtin, a class's
// field initializer, base and derived default constructors, generator, async and async-generator bodies, and one function
// both called and constructed. The modules' request point is request-kinds.mjs.
//
// The runs use the lane's default sequence, Producer --jitcache-delta-at-exit; Consumer (SPEC-integrator.harness.md
// section 7.4). Every run performs the same requests and prints the same results; the Producer makes each body hot
// enough to reach the baseline, where it is captured, and the Consumer checks that the first request of each kind
// imported.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const rounds = 400;

    // The first request of a kind imports in the Consumer. Each window holds one kind's first request. `imports` is the
    // count it expects: exact where the window holds nothing but the kind's own bodies, and at least the bodies the kind
    // has where it can also import a builtin or callback the body calls first.
    function firstRequest(label, imports, request)
    {
        const before = t.statistics();
        const value = request();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), { imports });
        return value;
    }

    const programText = "{ let total = 0; for (let i = 0; i < 3000; ++i) total = (total + i * 7) | 0; total; }";
    const indirectEvalText = "{ let total = 1; for (let i = 0; i < 3000; ++i) total = (total * 3 + i) | 0; total; }";
    const directEvalText = "{ let total = 2; for (let i = 0; i < 3000; ++i) total = (total ^ (i * 5)) | 0; total; }";
    const functionConstructorBody = "let total = 0; for (let i = 0; i < n; ++i) total = (total + i * i) | 0; return total;";

    function directEvalHost(text)
    {
        return eval(text);
    }

    // The field names are CommonIdentifiers, which the VM allocates when it is created (SPEC-integrator.harness.md
    // section 7.5). The twin check parses the imported field initializer again, which leaks its field names as the
    // native parse does, and main is imported too, so any other name would first be allocated by the decode of main's
    // core: a leak whose stack the Off run never reports.
    class FieldHolder {
        value = 3;
        second = this.value * 2;
    }
    class Base { }
    class Derived extends Base { }
    function* countUp(limit)
    {
        for (let i = 0; i < limit; ++i)
            yield i;
    }
    async function addLater(x)
    {
        await null;
        return x + 1;
    }
    async function* pairs()
    {
        yield 1;
        yield 2;
    }
    function both(x)
    {
        if (new.target) {
            this.value = x * 2;
            return;
        }
        return x + 1;
    }

    const results = [];
    results.push(`program ${firstRequest("program", [">=", 1], () => loadString(programText))}`);
    results.push(`indirect eval ${firstRequest("indirect eval", [">=", 1], () => (0, eval)(indirectEvalText))}`);
    results.push(`direct eval ${firstRequest("direct eval", [">=", 1], () => directEvalHost(directEvalText))}`);

    const fromConstructor = new Function("n", functionConstructorBody);
    let sum = firstRequest("Function constructor", [">=", 1], () => fromConstructor(10));
    for (let i = 0; i < rounds; ++i)
        sum = (sum + fromConstructor(i & 15)) | 0;
    results.push(`Function constructor ${sum}`);

    const values = [5, 8, 13, 21, 34];
    const isEven = value => !(value & 1);
    let found = firstRequest("builtin", [">=", 1], () => values.findLast(isEven));
    for (let i = 0; i < rounds; ++i)
        found = (found + values.findLast(isEven)) | 0;
    results.push(`builtin ${found}`);

    let fields = firstRequest("class fields", [">=", 2], () => new FieldHolder()).second;
    for (let i = 0; i < rounds; ++i)
        fields += new FieldHolder().second;
    results.push(`class fields ${fields}`);

    let derivedCount = firstRequest("default constructors", [">=", 2], () => new Derived()) instanceof Base ? 1 : 0;
    for (let i = 0; i < rounds; ++i)
        derivedCount += new Derived() instanceof Base ? 1 : 0;
    results.push(`default constructors ${derivedCount}`);

    // A generator's first next() also runs Generator.prototype.next and @generatorResume, which are JS builtins whose
    // bodies import at their own first request. One call of another function of each kind below makes those requests
    // first, whatever builtins the kind runs up to its first yield or await, so each window counts exactly its wrapper and
    // its body: a body that failed to import leaves the window one short.
    function* warmGenerator()
    {
        yield 0;
    }
    async function warmAsync()
    {
        await null;
    }
    async function* warmAsyncGenerator()
    {
        yield 0;
    }
    warmGenerator().next();
    warmAsync();
    warmAsyncGenerator().next();

    let generated = firstRequest("generator", 2, () => countUp(3).next().value);
    for (let i = 0; i < rounds; ++i) {
        for (const value of countUp(3))
            generated += value;
    }
    results.push(`generator ${generated}`);

    let awaited = 0;
    firstRequest("async", 2, () => addLater(1)).then(value => { awaited += value; });
    for (let i = 0; i < rounds; ++i)
        addLater(i).then(value => { awaited += value; });

    let yielded = 0;
    firstRequest("async generator", 2, () => pairs().next()).then(step => { yielded += step.value; });
    for (let i = 0; i < rounds; ++i)
        pairs().next().then(step => { yielded += step.value; });

    let called = firstRequest("called", [">=", 1], () => both(1));
    for (let i = 0; i < rounds; ++i)
        called = (called + both(i)) | 0;
    let constructed = firstRequest("constructed", [">=", 1], () => new both(2)).value;
    for (let i = 0; i < rounds; ++i)
        constructed = (constructed + new both(i).value) | 0;
    results.push(`called ${called}, constructed ${constructed}`);

    for (const line of results)
        print(line);
    Promise.resolve().then(() => Promise.resolve()).then(() => Promise.resolve()).then(() => {
        print(`async ${awaited}, async generator ${yielded}`);
        t.finish();
    });
})(...arguments);
