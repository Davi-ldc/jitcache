// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Every family of the cb.state format is captured with what the workload taught it, travels, and passes its twins in
// the Consumer. Each function's comment names the families it holds. The workload runs in rounds that each end with a
// full collection, whose finalization drains the array profiles and allocation hints of every LLInt and baseline CB, so
// a function captured at either capture point carries them drained, since capture reads only drained state. In twins
// builds the Producer also runs delta after each round and, once the rounds are done, reads back
// every committed cb.state, and the Consumer checks that the bodies it names were imported at their first call.
(function main(role) {
    const rounds = 12;
    const callsPerRound = 8;

    function weigh(item) {
        if (typeof item === "number")
            return item;
        if (typeof item === "string")
            return item.length;
        return item[0] + item[1];
    }

    // A0 get_length, A1 get_by_val, A2 in_by_val, A3 put_by_val, A4 put_by_val_direct (the element after the hole of
    // `[sum, , n]`), F16 new_array, F17 new_array_with_size, and F31 jneq_ptr: `new Array(n)` tests its callee against
    // Array and never jumps. Its calls of push, pop and indexOf are A11 call and A12 call_ignore_result sites, whose
    // `this` is an array; each site's own call data carries that profile, so cb.state holds no record for them.
    function arrayOps(array, slots, n) {
        let sum = 0;
        for (let i = 0; i < array.length; ++i)
            sum += array[i];
        if ((n - 1) in array)
            sum += 1;
        slots[n & 3] = sum;
        const sparse = [sum, , n];
        const sized = new Array(n);
        for (let i = 0; i < n; ++i)
            sized[i] = i + 0.5;
        array.push(n);
        array.pop();
        return sum + array.indexOf(n) + sparse.length + sized[n - 1];
    }

    // A13 tail_call, a strict call in tail position whose `this` is an array, which its own call data carries too.
    function tailAt(array, index) {
        "use strict";
        return array.at(index);
    }

    // A14 iterator_open, A15 iterator_next, F20 and F21. A site keeps at most two fast modes
    // (maxNumberOfFastIterationModes), so six copies of one loop take two kinds of iterable each, and the first two also a
    // user-defined iterable, which records Generic. Together they record every bit validation allows at both opcodes; a
    // plain array records FastArray at both.
    function iterateArraysAndMaps(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }
    function iterateSetsAndStrings(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }
    function iterateArrayValuesAndKeys(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }
    function iterateArrayEntriesAndMapKeys(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }
    function iterateMapValuesAndEntries(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }
    function iterateSetValuesAndEntries(iterable) {
        let total = 0;
        for (const item of iterable)
            total += weigh(item);
        return total;
    }

    // F24 iterator_close_check: a for-of over an array or a string that a return or a break leaves closes its iterator
    // through the check, which records the iterable's fast mode, FastArray or FastString.
    function firstHeavy(iterable, limit) {
        for (const item of iterable) {
            if (weigh(item) > limit)
                return item;
        }
        return null;
    }
    function countBefore(iterable, stop) {
        let count = 0;
        for (const item of iterable) {
            if (item === stop)
                break;
            ++count;
        }
        return count;
    }

    // F22 async_iterator_open and F23 async_iterator_next, at the for await below: an async generator takes
    // FastAsyncGenerator, a sync iterable AsyncFromSync, and a user-defined async iterable Generic. Code that awaits, as a
    // for await does, runs in a body function the async wrapper creates (FunctionNode::emitBytecode), so these entries
    // are in that body's CB and asyncTotal's own CB has none. asyncTotal is an async generator because the generator
    // object, which holds that body function, reaches the script, where an async function's stays internal; the readback
    // finds the body through it (resources/cb-sections.js, asyncGeneratorBody).
    async function* asyncTotal(source) {
        let total = 0;
        for await (const item of source)
            total += item;
        return total;
    }
    async function* countUp(limit) {
        for (let i = 0; i < limit; ++i)
            yield i;
    }

    // A5 to A9 and F25 to F29, the enumerator opcodes of for-in. A plain object enumerates in OwnStructureMode, an array
    // in IndexedMode, and an object with an enumerable inherited property in GenericMode. `other` has another structure,
    // so the get_by_val and put_by_val on it record HasSeenOwnStructureModeStructureMismatch. hasOwnProperty on the
    // enumerated object adds a wide jneq_ptr that never jumps.
    function enumerate(object, other) {
        let total = 0;
        for (const key in object) {
            total += object[key] | 0;
            if (key in other)
                total += 1;
            if (object.hasOwnProperty(key))
                total += 2;
            other[key] = total;
            total += other[key] & 7;
        }
        return total;
    }

    // F19 new_array_buffer, one literal of each copy-on-write type. Storing a double into the Int32 literal's arrays and
    // a string or an object into the Double one's and the second Int32 one's raises their hints past the literal's own
    // type; the string literal's arrays keep theirs.
    function literals(index) {
        const ints = [1, 2, 3];
        const doubles = [1.5, 2.5, 3.5];
        const strings = ["x", "y", "z"];
        const raised = [4, 5, 6];
        ints[index % 3] = 0.25;
        doubles[index % 3] = "s";
        raised[index % 3] = ints;
        return ints[0] + doubles.length + strings[index % 3].length + raised.length;
    }

    // F30 to_this: a sloppy function that reads `this` from receivers of two structures, which conflict.
    function readThis() {
        return this.weight;
    }

    // F31 jneq_ptr: `fn.call` and `fn.apply` test their callee against Function.prototype.call and apply, and an object
    // with methods of those names makes both jump.
    function invoke(fn, receiver, x) {
        return fn.call(receiver, x) + fn.apply(receiver, [x]);
    }
    function scaled(x) {
        return this.factor * x;
    }

    const double = x => x * 2;
    const isOdd = x => x & 1;
    const numbers = [3, 1, 4, 1, 5];
    const slots = [0, 0, 0, 0];
    const map = new Map([[1, 10], [2, 20], [3, 30]]);
    const set = new Set([7, 8, 9]);
    const text = "cb";
    const counted = {
        [Symbol.iterator]() {
            let i = 0;
            return { next: () => (i < 3 ? { value: i++, done: false } : { value: undefined, done: true }) };
        },
    };
    const asyncCounted = {
        [Symbol.asyncIterator]() {
            let i = 0;
            return { next: () => Promise.resolve(i < 2 ? { value: i++, done: false } : { value: undefined, done: true }) };
        },
    };
    const enumerated = [{ a: 1, b: 2, c: 3 }, [5, 6], Object.assign(Object.create({ inherited: 4 }), { own: 7 })];
    const other = { a: 0, b: 0, c: 0, 0: 0, 1: 0, own: 0, inherited: 0 };
    const receivers = [{ weight: 1, readThis }, { tag: 0, weight: 2, readThis }];
    const factorHolder = { factor: 3 };
    const impostor = {
        call(receiver, x) {
            return x + 1;
        },
        apply(receiver, args) {
            return args[0] + 2;
        },
    };
    // A hot get_by_val_with_this: `super[key]` in a method. Its array profile is no family, so it never travels.
    const base = { alpha: 1, beta: 2 };
    const derived = {
        __proto__: base,
        read(key) {
            return super[key];
        },
    };

    const checkingProducer = role === "Producer" && typeof jitcacheDelta === "function";
    const totals = { arrays: 0, tail: 0, iterate: 0, close: 0, async: 0, enumerate: 0, literals: 0, receivers: 0, invoke: 0, builtins: 0, super: 0 };
    for (let round = 0; round < rounds; ++round) {
        for (let i = 0; i < callsPerRound; ++i) {
            totals.arrays += arrayOps(numbers, slots, (i & 3) + 1);
            totals.tail += tailAt(numbers, i % numbers.length);
            totals.iterate += iterateArraysAndMaps(numbers) + iterateArraysAndMaps(map) + iterateArraysAndMaps(counted);
            totals.iterate += iterateSetsAndStrings(set) + iterateSetsAndStrings(text) + iterateSetsAndStrings(counted);
            totals.iterate += iterateArrayValuesAndKeys(numbers.values()) + iterateArrayValuesAndKeys(numbers.keys());
            totals.iterate += iterateArrayEntriesAndMapKeys(numbers.entries()) + iterateArrayEntriesAndMapKeys(map.keys());
            totals.iterate += iterateMapValuesAndEntries(map.values()) + iterateMapValuesAndEntries(map.entries());
            totals.iterate += iterateSetValuesAndEntries(set.values()) + iterateSetValuesAndEntries(set.entries());
            totals.close += firstHeavy(numbers, 3) + firstHeavy(text, 0).length + countBefore(numbers, 4) + countBefore(text, "b");
            for (const source of [countUp(3), [1, 2, 3], asyncCounted])
                asyncTotal(source).next().then(result => totals.async += result.value);
            for (const object of enumerated)
                totals.enumerate += enumerate(object, other);
            totals.literals += literals(i);
            totals.receivers += receivers[i & 1].readThis();
            totals.invoke += invoke(scaled, factorHolder, i) + invoke(impostor, null, i);
            totals.builtins += numbers.map(double).length + numbers.filter(isOdd).length;
            totals.super += derived.read(i & 1 ? "alpha" : "beta");
        }
        drainMicrotasks();
        fullGC();
        if (checkingProducer)
            jitcacheDelta();
    }
    print(JSON.stringify(totals));

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        for (const fn of [arrayOps, iterateArraysAndMaps, firstHeavy, countBefore, enumerate, literals, readThis, invoke])
            sections.expectImported(fn, "call", fn.name);
        sections.expectImported(Array.prototype.map, "call", "Array.prototype.map");
        const asyncBody = sections.asyncGeneratorBody(asyncTotal([]));
        if (asyncBody)
            sections.expectImported(asyncBody, "call", "asyncTotal's body");
        return;
    }
    if (!checkingProducer)
        return;

    const sections = load("./resources/cb-sections.js", "caller relative");
    const { IterationMode: Mode, EnumeratorMode, IndexingType, ToThisStatus } = sections;
    const stateOf = (fn, label) => sections.checkBody(fn, "call", label).state;
    function expectRecord(fn, label, family, what, predicate) {
        const records = sections.entries(stateOf(fn, label), family);
        if (!records.some(predicate))
            throw new Error(`${label}: no ${sections.familyNames[family]} record holds ${what}: ${sections.describe(records)}`);
    }
    const observed = record => record.modes !== 0;
    const raised = record => record.indexingType !== IndexingType.ArrayWithUndecided;

    for (const family of [0, 1, 2, 3, 4])
        expectRecord(arrayOps, "arrayOps", family, "an observed array mode", observed);
    expectRecord(arrayOps, "arrayOps", 16, "a hint past ArrayWithUndecided", raised);
    expectRecord(arrayOps, "arrayOps", 17, "a hint past ArrayWithUndecided", raised);
    expectRecord(arrayOps, "arrayOps", 31, "a jneq_ptr bit that stayed clear", record => record.bit === 0);
    expectRecord(Array.prototype.map, "Array.prototype.map", 10, "an observed array mode", observed);
    expectRecord(Array.prototype.map, "Array.prototype.map", 18, "a hint past ArrayWithUndecided", raised);
    expectRecord(iterateArraysAndMaps, "iterateArraysAndMaps", 14, "an observed array mode", observed);
    expectRecord(iterateArraysAndMaps, "iterateArraysAndMaps", 15, "an observed array mode", observed);
    // The call sites keep their array positions and no record.
    for (const [fn, family] of [[arrayOps, 11], [arrayOps, 12], [tailAt, 13]]) {
        const state = stateOf(fn, fn.name);
        if (!state.header.familyEntryCount[family] || sections.entries(state, family).length)
            throw new Error(`${fn.name}: ${sections.familyNames[family]} counts ${state.header.familyEntryCount[family]} entries and holds ${sections.entries(state, family).length} records`);
    }

    const sites = [
        [iterateArraysAndMaps, Mode.FastArray | Mode.FastMap | Mode.Generic, Mode.FastArray | Mode.FastMapEntries | Mode.Generic],
        [iterateSetsAndStrings, Mode.FastSet | Mode.FastString | Mode.Generic, Mode.FastSetValues | Mode.FastString | Mode.Generic],
        [iterateArrayValuesAndKeys, Mode.FastArrayValues | Mode.FastArrayKeys, Mode.FastArrayValues | Mode.FastArrayKeys],
        [iterateArrayEntriesAndMapKeys, Mode.FastArrayEntries | Mode.FastMapKeys, Mode.FastArrayEntries | Mode.FastMapKeys],
        [iterateMapValuesAndEntries, Mode.FastMapValues | Mode.FastMapEntries, Mode.FastMapValues | Mode.FastMapEntries],
        [iterateSetValuesAndEntries, Mode.FastSetValues | Mode.FastSetEntries, Mode.FastSetValues | Mode.FastSetEntries],
    ];
    let openModes = 0;
    let nextModes = 0;
    for (const [fn, open, next] of sites) {
        const state = stateOf(fn, fn.name);
        const [openRecord] = sections.entries(state, 20);
        const [nextRecord] = sections.entries(state, 21);
        if (!openRecord || (openRecord.modes & open) !== open)
            throw new Error(`${fn.name}: iterator_open records ${sections.describe(openRecord)}, expected the modes 0x${open.toString(16)}`);
        if (!nextRecord || (nextRecord.modes & next) !== next)
            throw new Error(`${fn.name}: iterator_next records ${sections.describe(nextRecord)}, expected the modes 0x${next.toString(16)}`);
        openModes |= openRecord.modes;
        nextModes |= nextRecord.modes;
    }
    if (openModes !== 0x1fff || nextModes !== 0x1ff3)
        throw new Error(`the six sites record 0x${openModes.toString(16)} at iterator_open and 0x${nextModes.toString(16)} at iterator_next, not every native bit`);

    // The loops a return or a break leaves: their iterator_close_check entries record FastArray and FastString, every bit
    // validation allows there.
    for (const fn of [firstHeavy, countBefore]) {
        const records = sections.entries(stateOf(fn, fn.name), 24);
        const closeModes = records.reduce((modes, record) => modes | record.modes, 0);
        if (closeModes !== (Mode.FastArray | Mode.FastString))
            throw new Error(`${fn.name}: iterator_close_check records ${sections.describe(records)}, expected FastArray and FastString`);
    }

    // The for await's entries, in asyncTotal's body: every bit validation allows at each opcode.
    const asyncBody = sections.asyncGeneratorBody(asyncTotal([]));
    if (asyncBody) {
        const asyncOpen = Mode.FastAsyncGenerator | Mode.AsyncFromSync | Mode.Generic;
        const asyncNext = Mode.FastAsyncGenerator | Mode.Generic;
        expectRecord(asyncBody, "asyncTotal's body", 22, "FastAsyncGenerator, AsyncFromSync and Generic alone", record => record.modes === asyncOpen);
        expectRecord(asyncBody, "asyncTotal's body", 23, "FastAsyncGenerator and Generic alone", record => record.modes === asyncNext);
    }

    const everyMode = EnumeratorMode.Indexed | EnumeratorMode.OwnStructure | EnumeratorMode.Generic;
    expectRecord(enumerate, "enumerate", 25, "every enumeration mode", record => (record.modes & everyMode) === everyMode);
    expectRecord(enumerate, "enumerate", 26, "a mode", observed);
    expectRecord(enumerate, "enumerate", 27, "a mode", observed);
    expectRecord(enumerate, "enumerate", 28, "an own-structure mismatch", record => (record.modes & EnumeratorMode.OwnStructureMismatch) !== 0);
    expectRecord(enumerate, "enumerate", 29, "an own-structure mismatch", record => (record.modes & EnumeratorMode.OwnStructureMismatch) !== 0);

    const hints = sections.entries(stateOf(literals, "literals"), 19).map(record => record.indexingType);
    const expectedHints = [IndexingType.CopyOnWriteArrayWithDouble, IndexingType.CopyOnWriteArrayWithContiguous, IndexingType.CopyOnWriteArrayWithContiguous, IndexingType.CopyOnWriteArrayWithContiguous];
    if (hints.join() !== expectedHints.join())
        throw new Error(`literals: new_array_buffer hints ${sections.describe(hints)}, expected ${sections.describe(expectedHints)}`);

    expectRecord(readThis, "readThis", 30, "ToThisConflicted", record => record.status === ToThisStatus.Conflicted);
    expectRecord(invoke, "invoke", 31, "a jneq_ptr bit that jumped", record => record.bit === 1);
})(...arguments);
