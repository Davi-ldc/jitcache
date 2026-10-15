// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Iteration in imported images, each import twin-checked. for-of over arrays, maps, sets, strings and generators runs
// iterator_open and iterator_next, whose done test branches on truthiness against the empty string
// (VMCell::EmptyString) and whose generic paths call through JITSlowPathCall and the IC slow-path thunk; spread runs a
// slow op; for await over an async generator runs async_iterator_next, which passes the VM's syncResumeCallCache, in the
// body asyncSums resumes, which an async generator keeps where a builtin can read it, unlike an async function; for-in
// runs the enumerator opcodes, which decode structure IDs. JIT::emit_op_enumerator_next emits the own-structure fast
// path, whose end of the names stores the small strings' sentinel, only when the LLInt saw that mode alone, so ownKeysOf
// sees only plain objects whose enumerable properties are their own and none an index, while keysOf also meets an
// array, an inherited property and a null prototype. Every loop tests the VM's trap bits, calls the check-traps handler
// on its slow path, and on its loop hint's slow path passes topEntryFrame and the VM.
// The consumer checks that its images carry these targets, and an IC slow-path call for each access type their caches
// use.
(function main(role, scratch, artifact, sequence) {
    function sumOf(iterable) {
        let total = 0;
        for (const value of iterable)
            total += typeof value === "number" ? value : value.length;
        return total;
    }
    function* range(n) {
        for (let i = 0; i < n; ++i)
            yield i;
    }
    function spreadCount(iterable) {
        return [...iterable].length;
    }
    function keysOf(object) {
        const keys = [];
        for (const key in object)
            keys.push(key);
        return keys.join(",");
    }
    function ownKeysOf(object) {
        let keys = "";
        for (const key in object)
            keys += key;
        return keys;
    }
    function destructure(pair) {
        const [first, second = "none"] = pair;
        return `${first}:${second}`;
    }
    async function* asyncRange(n) {
        for (let i = 0; i < n; ++i)
            yield i;
    }
    async function* asyncSums(n) {
        let total = 0;
        for await (const value of asyncRange(n))
            total += value;
        for await (const value of [n, Promise.resolve(n)])
            total += value;
        yield total;
    }

    function Base() {
        this.own = 1;
    }
    Base.prototype.inherited = 2;
    const objects = [
        { a: 1, b: 2, c: 3 },
        [10, 20, 30],
        new Base(),
        Object.assign(Object.create(null), { z: 1, y: 2 }),
        { 2: "two", 1: "one", name: "named" },
    ];

    let total = 0;
    let text = "";
    const asyncResults = [];
    for (let i = 0; i < 60; ++i) {
        total += sumOf([i, i + 1, i + 2]);
        total += sumOf(new Map([[i, i], [i + 1, i]]).keys());
        total += sumOf(new Set([i, i, i + 1]));
        total += sumOf(range(i % 5));
        total += sumOf("abc".slice(i % 3));
        total += spreadCount(range(i % 4)) + spreadCount("text");
        text += keysOf(objects[i % objects.length]) + ";";
        text += ownKeysOf(i % 2 ? { first: i, second: 2 } : { third: 3, fourth: i, fifth: 5 }) + ";";
        text += destructure(i % 2 ? [i] : [i, i + 1]) + ";";
        asyncSums(i % 6).next().then(result => asyncResults.push(result.value));
    }
    drainMicrotasks();
    print(total);
    print(text);
    print(asyncResults.join(","));

    // The case happened (HARNESS.md, Tests): the producer compiled every function the loop calls, and the bodies the
    // generators' and async generators' outer functions create and resume, so each ran its import here, and their images
    // carry the targets the header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { sumOf, range, spreadCount, keysOf, ownKeysOf, destructure, asyncRange, asyncSums };
        if (sections.checksImports(role)) {
            bodies["range's body"] = sections.generatorBody(range(0));
            bodies["asyncRange's body"] = sections.generatorBody(asyncRange(0));
            bodies["asyncSums's body"] = sections.generatorBody(asyncSums(0));
        }
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "A7, E6", kind: "VMCell", a: "EmptyString", form: "Pointer" },
            { site: "D9", kind: "VMCell", a: "SmallStringsSentinel", form: "Pointer" },
            { site: "D10, A6", kind: "StructureIDBase", form: "Pointer" },
            { site: "E4, D1", kind: "InlineCacheSlowPathThunk", form: "Call", eachMoldAccessType: true },
            { site: "E5, A1", kind: "SlowPathThunk", form: "Call" },
            { site: "E7", kind: "VMAddress", a: "SyncResumeCallCache", form: "Pointer" },
            { site: "C14", kind: "VMAddress", a: "TrapBits", form: "Pointer" },
            { site: "C15", kind: "BaselineThunk", a: "OpCheckTrapsHandler", form: "Call" },
            { site: "C18, A10", kind: "VMAddress", a: "TopEntryFrame", form: "Pointer" },
            { site: "C19", kind: "VMAddress", a: "VM", form: "Pointer" },
        ]);
    }
})(...arguments);
