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
// iterator_next and iterator_close_check compare the iterator with the VM's fast-array and fast-string sentinels.
// firstTwo and firstPair iterate int32, contiguous, double and holey arrays and strings: int32 and contiguous arrays
// take iterator_next's inline fast-array path, a double or a hole calls operationIteratorNextFastArray, and a string
// operationIteratorNextFastString. Both leave after two values, so iterator_close_check meets both sentinels while the
// protocol watchpoints hold. Last, breakAfterInstallingReturn's loop installs a return method on
// %ArrayIteratorPrototype% before its break, which fires the array iterator protocol watchpoint after iterator_open
// chose the fast path, so the check takes slow_path_iterator_close_check and the break calls the new method.
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
    function firstTwo(iterable) {
        let taken = 0;
        let total = 0;
        for (const value of iterable) {
            total += typeof value === "number" ? value : value === undefined ? 100 : value.length;
            if (++taken === 2)
                break;
        }
        return total;
    }
    function firstPair(iterable) {
        const [first, second] = iterable;
        return `${first}|${second}`;
    }
    const arrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const closedBy = [];
    function breakAfterInstallingReturn(array, install) {
        let total = 0;
        for (const value of array) {
            total += value;
            if (install) {
                arrayIteratorPrototype.return = function () {
                    closedBy.push(total);
                    return { done: true };
                };
                break;
            }
        }
        return total;
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
        const shapes = [[i, i + 1, i + 2], ["a", "bc", "def"], [i + 0.5, 1.5, 2.5], [i, , i + 2], "wxyz".slice(i % 3)];
        for (const shape of shapes) {
            total += firstTwo(shape);
            text += firstPair(shape) + ";";
        }
        total += breakAfterInstallingReturn([i, 1, 2], false);
    }
    drainMicrotasks();
    print(total);
    print(text);
    print(asyncResults.join(","));

    // The protocol watchpoint fires inside the loop, after the iteration chose the fast path; the method goes again at
    // once, while the watchpoint stays fired for the rest of the run.
    print(breakAfterInstallingReturn([7, 8, 9], true));
    delete arrayIteratorPrototype.return;
    print(closedBy.join(","));

    // The case happened: the producer compiled every function the loop calls, and the bodies the generators' and async
    // generators' outer functions create and resume, so each ran its import here, and their images carry the targets the
    // header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = {
            sumOf, range, spreadCount, keysOf, ownKeysOf, destructure, asyncRange, asyncSums, firstTwo, firstPair,
            breakAfterInstallingReturn,
        };
        if (sections.checksImports(role)) {
            bodies["range's body"] = sections.generatorBody(range(0));
            bodies["asyncRange's body"] = sections.generatorBody(asyncRange(0));
            bodies["asyncSums's body"] = sections.generatorBody(asyncSums(0));
        }
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "iterator_next's empty-string test", kind: "VMCell", a: "EmptyString", form: "Pointer" },
            { site: "enumerator_next's sentinel string store", kind: "VMCell", a: "SmallStringsSentinel", form: "Pointer" },
            { site: "the iteration opcodes' fast-array test", kind: "VMCell", a: "FastArraySentinel", form: "Pointer" },
            { site: "the iteration opcodes' fast-string test", kind: "VMCell", a: "FastStringSentinel", form: "Pointer" },
            { site: "the enumerator opcodes' structure ID decode", kind: "StructureIDBase", form: "Pointer" },
            {
                site: "a slow case's call to its inline cache's slow-path code",
                kind: "InlineCacheSlowPathThunk", form: "Call", eachMoldAccessType: true,
            },
            { site: "iterator_open's try-fast slow-path call", kind: "SlowPathThunk", form: "Call" },
            { site: "async_iterator_next's sync resume call cache", kind: "VMAddress", a: "SyncResumeCallCache", form: "Pointer" },
            { site: "check_traps's trap bits test", kind: "VMAddress", a: "TrapBits", form: "Pointer" },
            { site: "check_traps's handler thunk call", kind: "BaselineThunk", a: "OpCheckTrapsHandler", form: "Call" },
            { site: "loop_hint's entry frame load", kind: "VMAddress", a: "TopEntryFrame", form: "Pointer" },
            { site: "operationOptimize's VM argument", kind: "VMAddress", a: "VM", form: "Pointer" },
        ]);
    }
})(...arguments);
