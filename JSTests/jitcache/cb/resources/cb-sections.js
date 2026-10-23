// Reads the CB lane's two sections of a committed body, cb.state and cb.summary, and checks the summary against the
// state captured with it.
//
// Only twins builds have the shell functions this file calls, so a script loads it only when they exist and only
// outside the Off role: load() returns the object at the end of this file and defines no global. asyncGeneratorBody also
// needs $vm, which the runner gives twins-mode runs alone.
(function () {
    "use strict";

    // The shared byte reader. Its file defines no global either, so this load leaves the global object as it was.
    const { byteReader } = load("../../resources/jitcache.js", "caller relative");

    const stateHeaderSize = 160;
    const summaryHeaderSize = 24;
    const lazyOperandRecordSize = 24;

    // The families of the cb.state format, in its index order.
    const familyNames = [
        "A0 get_length", "A1 get_by_val", "A2 in_by_val", "A3 put_by_val", "A4 put_by_val_direct",
        "A5 enumerator_next", "A6 enumerator_get_by_val", "A7 enumerator_in_by_val", "A8 enumerator_put_by_val",
        "A9 enumerator_has_own_property", "A10 new_array_with_species", "A11 call", "A12 call_ignore_result",
        "A13 tail_call", "A14 iterator_open", "A15 iterator_next",
        "F16 new_array", "F17 new_array_with_size", "F18 new_array_with_species", "F19 new_array_buffer",
        "F20 iterator_open", "F21 iterator_next", "F22 async_iterator_open", "F23 async_iterator_next",
        "F24 iterator_close_check",
        "F25 enumerator_next", "F26 enumerator_in_by_val", "F27 enumerator_has_own_property",
        "F28 enumerator_put_by_val", "F29 enumerator_get_by_val", "F30 to_this", "F31 jneq_ptr",
    ];
    const numberOfFamilies = familyNames.length;

    // The family ranges of cb.state's per-family arrays, from the array profiles to the jneq_ptr bits, as [first, end).
    // The call sites A11..A13 count as array positions, which pair cb.summary's array flags with the UnlinkedCodeBlock's
    // copies, and hold no cb.state record: the ICs lane carries their this profile in their own CallSiteData.
    const arrayProfileFamilies = [0, 16];
    const callSiteFamilies = [11, 14];
    const hintFamilies = [16, 20];
    const iterationFamilies = [20, 25];
    const enumeratorFamilies = [25, 30];
    const toThisFamilies = [30, 31];
    const branchFamilies = [31, 32];
    const isCallSiteFamily = family => family >= callSiteFamilies[0] && family < callSiteFamilies[1];

    const CounterMode = { NotCarried: 0, Carried: 1 };
    const OperandKind = { Argument: 0, Local: 1, Tmp: 2 };
    // The to_this statuses: ToThisOK, ToThisConflicted and ToThisClearedByGC.
    const ToThisStatus = { OK: 0, Conflicted: 1, ClearedByGC: 2 };

    const IterationMode = {
        Generic: 1 << 0, FastArray: 1 << 1, FastMap: 1 << 2, FastSet: 1 << 3, FastString: 1 << 4,
        FastArrayValues: 1 << 5, FastArrayKeys: 1 << 6, FastArrayEntries: 1 << 7,
        FastMapKeys: 1 << 8, FastMapValues: 1 << 9, FastMapEntries: 1 << 10,
        FastSetValues: 1 << 11, FastSetEntries: 1 << 12,
        FastAsyncGenerator: 1 << 13, AsyncFromSync: 1 << 14,
    };
    const EnumeratorMode = { Indexed: 1, OwnStructure: 2, Generic: 4, OwnStructureMismatch: 8 };
    const IndexingType = {
        ArrayWithUndecided: 0x03, ArrayWithInt32: 0x05, ArrayWithDouble: 0x07, ArrayWithContiguous: 0x09,
        CopyOnWriteArrayWithInt32: 0x15, CopyOnWriteArrayWithDouble: 0x17, CopyOnWriteArrayWithContiguous: 0x19,
    };

    const sum = (counts, [first, end]) => {
        let total = 0;
        for (let family = first; family < end; ++family)
            total += counts[family];
        return total;
    };
    // The entries of a range's families that have cb.state records, which leaves out the call sites.
    const recordCount = (counts, [first, end]) => {
        let total = 0;
        for (let family = first; family < end; ++family) {
            if (!isCallSiteFamily(family))
                total += counts[family];
        }
        return total;
    };

    // Walks one family range of an array whose entries follow the families in table order, tagging each entry with its
    // family. An array profile also gets its array position, which counts the call sites that have no record.
    function readFamilies(counts, range, offset, size, readEntry) {
        const records = [];
        let at = offset;
        let position = 0;
        for (let family = range[0]; family < range[1]; ++family) {
            for (let index = 0; index < counts[family]; ++index, ++position) {
                if (isCallSiteFamily(family))
                    continue;
                const record = { family, ...readEntry(at) };
                if (range === arrayProfileFamilies)
                    record.position = position;
                records.push(record);
                at += size;
            }
        }
        return records;
    }

    // cb.state. Returns the parsed section, or, for a section too short for its header or for the arrays its header lays
    // out, the section with that problem and nothing else.
    function parseState(buffer) {
        const read = byteReader(buffer);
        const parsed = { byteLength: buffer.byteLength, problems: [] };
        if (buffer.byteLength < stateHeaderSize) {
            parsed.problems.push(`cb.state: ${buffer.byteLength} bytes hold no StateHeader`);
            return parsed;
        }
        const familyEntryCount = [];
        for (let family = 0; family < numberOfFamilies; ++family)
            familyEntryCount.push(read.u32(32 + 4 * family));
        const header = {
            tier: read.u8(0),
            counterMode: read.u8(1),
            numArguments: read.u32(4),
            numValueProfiles: read.u32(8),
            numLazyOperandProfiles: read.u32(12),
            optimizationDelayCounter: read.u16(16),
            reoptimizationRetryCounter: read.u16(18),
            counterValue: read.i32(20),
            counterTotalCount: read.f32(24),
            counterActiveThreshold: read.i32(28),
            familyEntryCount,
        };
        parsed.header = header;

        const layout = {};
        let end = stateHeaderSize;
        const place = (name, count, size) => {
            layout[name] = end;
            end += count * size;
        };
        place("arguments", header.numArguments, 8);
        place("values", header.numValueProfiles, 8);
        place("arrayProfiles", recordCount(familyEntryCount, arrayProfileFamilies), 8);
        place("lazyOperands", header.numLazyOperandProfiles, lazyOperandRecordSize);
        place("hints", recordCount(familyEntryCount, hintFamilies), 2);
        place("iterationModes", recordCount(familyEntryCount, iterationFamilies), 2);
        place("enumeratorModes", recordCount(familyEntryCount, enumeratorFamilies), 1);
        place("toThis", familyEntryCount[toThisFamilies[0]], 1);
        place("branchBits", familyEntryCount[branchFamilies[0]], 1);
        if (buffer.byteLength < end) {
            parsed.problems.push(`cb.state has ${buffer.byteLength} bytes, fewer than the ${end} its header lays out`);
            return parsed;
        }

        const predictions = (offset, count) => {
            const values = [];
            for (let index = 0; index < count; ++index)
                values.push(read.u64(offset + 8 * index));
            return values;
        };
        parsed.arguments = predictions(layout.arguments, header.numArguments);
        parsed.values = predictions(layout.values, header.numValueProfiles);
        parsed.arrayProfiles = readFamilies(familyEntryCount, arrayProfileFamilies, layout.arrayProfiles, 8, offset => ({
            modes: read.u32(offset),
            flags: read.u32(offset + 4),
        }));
        parsed.lazyOperands = [];
        for (let index = 0; index < header.numLazyOperandProfiles; ++index) {
            const offset = layout.lazyOperands + lazyOperandRecordSize * index;
            parsed.lazyOperands.push({
                bytecodeIndexBits: read.u32(offset),
                kind: read.u32(offset + 4),
                value: read.i32(offset + 8),
                prediction: read.u64(offset + 16),
            });
        }
        parsed.hints = readFamilies(familyEntryCount, hintFamilies, layout.hints, 2, offset => {
            const bits = read.u16(offset);
            return { indexingType: bits >> 8, vectorLength: bits & 0xff };
        });
        parsed.iterationModes = readFamilies(familyEntryCount, iterationFamilies, layout.iterationModes, 2, offset => ({ modes: read.u16(offset) }));
        parsed.enumeratorModes = readFamilies(familyEntryCount, enumeratorFamilies, layout.enumeratorModes, 1, offset => ({ modes: read.u8(offset) }));
        parsed.toThis = readFamilies(familyEntryCount, toThisFamilies, layout.toThis, 1, offset => ({ status: read.u8(offset) }));
        parsed.branchBits = readFamilies(familyEntryCount, branchFamilies, layout.branchBits, 1, offset => ({ bit: read.u8(offset) }));
        return parsed;
    }

    // cb.summary.
    function parseSummary(buffer) {
        const read = byteReader(buffer);
        const parsed = { byteLength: buffer.byteLength, problems: [] };
        if (buffer.byteLength < summaryHeaderSize) {
            parsed.problems.push(`cb.summary: ${buffer.byteLength} bytes hold no SummaryHeader`);
            return parsed;
        }
        const header = {
            tier: read.u8(0),
            counterMode: read.u8(1),
            numArguments: read.u32(4),
            numValueProfiles: read.u32(8),
            numArrayProfiles: read.u32(12),
            numLazyOperandProfiles: read.u32(16),
            counterProgress: read.u32(20),
        };
        parsed.header = header;
        const categories = header.numArguments + header.numValueProfiles + header.numLazyOperandProfiles;
        const end = summaryHeaderSize + 8 * categories + header.numArrayProfiles;
        if (buffer.byteLength < end) {
            parsed.problems.push(`cb.summary has ${buffer.byteLength} bytes, fewer than the ${end} its header lays out`);
            return parsed;
        }
        let offset = summaryHeaderSize;
        const take = count => {
            const values = [];
            for (let index = 0; index < count; ++index, offset += 8)
                values.push(read.u64(offset));
            return values;
        };
        parsed.arguments = take(header.numArguments);
        parsed.values = take(header.numValueProfiles);
        parsed.lazyOperands = take(header.numLazyOperandProfiles);
        parsed.arrayFlags = [];
        for (let index = 0; index < header.numArrayProfiles; ++index)
            parsed.arrayFlags.push(read.u8(offset + index));
        return parsed;
    }

    const hex = value => `0x${value.toString(16)}`;

    // The summary's counterProgress, as capture computes it from the counter fields of cb.state.
    function counterProgressOf(header) {
        if (header.counterMode !== CounterMode.Carried)
            return 0;
        const progress = header.counterTotalCount + header.counterValue;
        if (progress <= 0)
            return 0;
        return Math.min(Math.floor(progress), header.counterActiveThreshold);
    }

    // The summary against the state of the same capture: equal counts and counter mode, every slot a superset of its
    // cb.state slot (equal for lazy operands, which have no UCB copy), every array flag byte at a record's array position a
    // superset of the CB's flags, and the counterProgress capture computes. A call site's flag byte has no record to cover.
    function summaryProblems(summary, state) {
        const problems = [...summary.problems];
        const header = summary.header;
        if (!header || problems.length || !state.header || state.problems.length)
            return problems;

        const stateHeader = state.header;
        const arrayProfileCount = sum(stateHeader.familyEntryCount, arrayProfileFamilies);
        if (header.counterMode !== stateHeader.counterMode)
            problems.push(`the summary's counter mode ${header.counterMode} differs from cb.state's ${stateHeader.counterMode}`);
        if (header.numArguments !== stateHeader.numArguments || header.numValueProfiles !== stateHeader.numValueProfiles
            || header.numLazyOperandProfiles !== stateHeader.numLazyOperandProfiles || header.numArrayProfiles !== arrayProfileCount) {
            problems.push("the summary's counts differ from cb.state's");
            return problems;
        }
        const covers = (name, categories, predictions) => {
            predictions.forEach((prediction, index) => {
                if (prediction & ~categories[index])
                    problems.push(`summary ${name} category ${index} ${hex(categories[index])} misses cb.state's ${hex(prediction)}`);
            });
        };
        covers("argument", summary.arguments, state.arguments);
        covers("value", summary.values, state.values);
        state.lazyOperands.forEach((record, index) => {
            if (summary.lazyOperands[index] !== record.prediction)
                problems.push(`summary lazy-operand category ${index} ${hex(summary.lazyOperands[index])} is not cb.state's ${hex(record.prediction)}`);
        });
        state.arrayProfiles.forEach(record => {
            if (record.flags & ~summary.arrayFlags[record.position])
                problems.push(`summary array flags ${record.position} ${hex(summary.arrayFlags[record.position])} miss cb.state's ${hex(record.flags)}`);
        });
        const progress = counterProgressOf(stateHeader);
        if (header.counterProgress !== progress)
            problems.push(`summary counterProgress ${header.counterProgress}, and cb.state's counter gives ${progress}`);
        return problems;
    }

    // The records of one family, in metadata-ID order.
    function entries(state, family) {
        const records = [...state.arrayProfiles, ...state.hints, ...state.iterationModes, ...state.enumeratorModes, ...state.toThis, ...state.branchBits];
        return records.filter(record => record.family === family);
    }

    // The committed body of fn's CB of kind ("call" or "construct"), both sections parsed, or null when no body holds them.
    function readBody(fn, kind) {
        const key = globalThis.jitcacheBodyKey(fn, kind);
        if (!key)
            return null;
        const stateBuffer = globalThis.jitcacheReadSection(key, "cb.state");
        const summaryBuffer = globalThis.jitcacheReadSection(key, "cb.summary");
        if (!stateBuffer || !summaryBuffer)
            return null;
        return { key, state: parseState(stateBuffer), summary: parseSummary(summaryBuffer) };
    }

    // readBody, which must find a body whose sections parse and agree; throws otherwise.
    function checkBody(fn, kind, label) {
        const body = readBody(fn, kind);
        if (!body)
            throw new Error(`${label}: no committed body holds cb.state and cb.summary`);
        const problems = [...body.state.problems, ...summaryProblems(body.summary, body.state)];
        if (problems.length)
            throw new Error(`${label}: ${problems.join("; ")}`);
        return body;
    }

    // Rewrites the counter of a committed body that carries one, `body` being what readBody or checkBody returned
    // (jitcacheRewriteSection): cb.state's counterValue and counterTotalCount, which sit back to back at offset 20, and
    // cb.summary's counterProgress at offset 20, which capture computes from them, so the summary still matches the state.
    function rewriteCounter(body, counterValue, counterTotalCount) {
        const header = body.state.header;
        if (!header || header.counterMode !== CounterMode.Carried)
            throw new Error(`rewriteCounter: the body carries no counter: ${describe(header)}`);
        const stateBytes = new DataView(new ArrayBuffer(8));
        stateBytes.setInt32(0, counterValue, true);
        stateBytes.setFloat32(4, counterTotalCount, true);
        globalThis.jitcacheRewriteSection(body.key, "cb.state", 20, Array.from(new Uint8Array(stateBytes.buffer)));
        const progress = counterProgressOf({ ...header, counterValue, counterTotalCount: Math.fround(counterTotalCount) });
        const summaryBytes = new DataView(new ArrayBuffer(4));
        summaryBytes.setUint32(0, progress, true);
        globalThis.jitcacheRewriteSection(body.key, "cb.summary", 20, Array.from(new Uint8Array(summaryBytes.buffer)));
    }

    // The body function of an async generator object: the function its wrapper created and stored in the object's
    // internal field JSAsyncGenerator::Field::Next, index 1 (runtime/JSAsyncGenerator.h). The generator's code, a for await
    // included, lives in that function's CB and not in the wrapper's (FunctionNode::emitBytecode), and no script can name
    // the function, so this reads the field through a function the shell compiles as a builtin, which may call
    // @getInternalField. Null when the run has no $vm.
    let readAsyncGeneratorNext = null;
    function asyncGeneratorBody(generator) {
        const dollarVM = globalThis.$vm;
        if (typeof dollarVM !== "object" || !dollarVM)
            return null;
        readAsyncGeneratorNext ??= dollarVM.createBuiltin("(function (generator) { return @getInternalField(generator, 1); })");
        const body = readAsyncGeneratorNext(generator);
        if (typeof body !== "function")
            throw new Error(`asyncGeneratorBody: internal field 1 of the generator holds ${String(body)}, not its body function`);
        return body;
    }

    // A record for an error message; predictions are BigInts, which JSON.stringify refuses.
    const describe = value => JSON.stringify(value, (name, field) => typeof field === "bigint" ? hex(field) : field);

    // For a Consumer: the CB of fn's kind installed an imported body when it was created, so its body never ran in the
    // LLInt and never compiled to baseline in this process; throws otherwise.
    function expectImported(fn, kind, label) {
        const counts = globalThis.jitcacheBodyEvents(fn, kind);
        if (!counts || counts.llintInstructions || counts.baselineCompiles)
            throw new Error(`${label} was not imported at its first ${kind}: ${describe(counts)}`);
    }

    return Object.freeze({
        familyNames, CounterMode, OperandKind, IterationMode, EnumeratorMode, IndexingType, ToThisStatus,
        parseState, parseSummary, summaryProblems, counterProgressOf, entries,
        readBody, checkBody, rewriteCounter, asyncGeneratorBody, describe, expectImported,
    });
})();
