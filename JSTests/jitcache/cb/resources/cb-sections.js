// Reads the CB lane's two sections of a committed body, cb.state and cb.summary (SPEC-cb.md sections 3.2 and 3.3), and
// checks them against every rule of section 3.4 that needs no CodeBlock, and the summary against the state it was built
// from (I8, I10, I16). The rules that compare a count with the CB (V2 to V4, V8's instruction walk and V13's offset bound)
// are SC1's, which every strict capture applies before the writer sees the bytes.
//
// Only twins builds have the shell functions this file calls (harness sub-SPEC section 5.2), so a script loads it only
// when they exist and only outside the Off role: load() returns the object at the end of this file and defines no global.
// asyncGeneratorBody also needs $vm, which the runner gives twins-mode runs alone (section 5.4).
(function () {
    "use strict";

    const stateHeaderSize = 160;
    const summaryHeaderSize = 24;
    const lazyOperandRecordSize = 24;

    // Section 3.1's family table, in the format's index order.
    const familyNames = [
        "A0 get_length", "A1 get_by_val", "A2 in_by_val", "A3 put_by_val", "A4 put_by_val_direct",
        "A5 enumerator_next", "A6 enumerator_get_by_val", "A7 enumerator_in_by_val", "A8 enumerator_put_by_val",
        "A9 enumerator_has_own_property", "A10 new_array_with_species", "A11 call", "A12 call_ignore_result",
        "A13 tail_call", "A14 iterator_open", "A15 iterator_next",
        "F16 new_array", "F17 new_array_with_size", "F18 new_array_with_species", "F19 new_array_buffer",
        "F20 iterator_open", "F21 iterator_next", "F22 async_iterator_open", "F23 async_iterator_next",
        "F24 enumerator_next", "F25 enumerator_in_by_val", "F26 enumerator_has_own_property",
        "F27 enumerator_put_by_val", "F28 enumerator_get_by_val", "F29 to_this", "F30 jneq_ptr",
    ];
    const numberOfFamilies = familyNames.length;

    // The family ranges of arrays 3 and 5 to 9 of section 3.2, as [first, end).
    const arrayProfileFamilies = [0, 16];
    const hintFamilies = [16, 20];
    const iterationFamilies = [20, 24];
    const enumeratorFamilies = [24, 29];
    const toThisFamilies = [29, 30];
    const branchFamilies = [30, 31];

    const CounterMode = { NotCarried: 0, Carried: 1 };
    const OperandKind = { Argument: 0, Local: 1, Tmp: 2 };
    const int32Max = 0x7fffffff;

    // The domains the V-rules bound, with the engine's values at this pin.
    // SpecBytecodeTop: every SpeculatedType bit below bit 51 except the unused bit 3, the two Int52 bits 39 and 40, and
    // SpecDoubleImpureNaN, bit 44 (bytecode/SpeculatedType.h).
    const specBytecodeTop = ((1n << 51n) - 1n) & ~((1n << 3n) | (1n << 39n) | (1n << 40n) | (1n << 44n));
    // ALL_ARRAY_MODES: every array mode bit except 2, 14, 15 and 24, which no indexing type or typed array maps to
    // (bytecode/ArrayProfile.h).
    const allArrayModes = 0xfeff3ffb;
    const arrayProfileFlagMask = 0xff;
    // V8: ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage and
    // ArrayWithSlowPutArrayStorage in F16..F18; the copy-on-write Int32, Double and Contiguous types in F19, in their order
    // (runtime/IndexingType.h).
    const writableArrayHints = [0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d];
    const copyOnWriteHints = [0x15, 0x17, 0x19];
    const maximumVectorLengthHint = 25; // BASE_CONTIGUOUS_VECTOR_LEN_MAX
    // V9, per family F20..F23.
    const nativeIterationModes = [0x1fff, 0x1ff1, 0x6001, 0x2001];
    // V10: IndexedMode, OwnStructureMode, GenericMode and HasSeenOwnStructureModeStructureMismatch.
    const enumeratorModeMask = 0x0f;
    // V11: ToThisOK, ToThisConflicted and ToThisClearedByGC.
    const ToThisStatus = { OK: 0, Conflicted: 1, ClearedByGC: 2 };
    // V14: the defaults of maximumOptimizationDelay and reoptimizationRetryCounterMax, both fixed (options.md).
    const maximumOptimizationDelay = 5;
    const reoptimizationRetryCounterMax = 21;
    // V13: VirtualRegister::invalidVirtualRegister and FirstConstantRegisterIndex.
    const invalidVirtualRegister = 0x3fffffff;
    const firstConstantRegisterIndex = 0x40000000;
    // V13 and BytecodeIndex: the offset sits above the two checkpoint bits, and an offset of 2^30 - 1 is the hash table's
    // empty or deleted value, which no instruction stream reaches.
    const checkpointBits = 2;
    const unreachableOffset = 0x3fffffff;

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
    const roundUpToEight = size => Math.ceil(size / 8) * 8;

    // Walks one family range of an array whose entries follow the families in table order, tagging each entry with its
    // family.
    function readFamilies(counts, range, offset, size, read) {
        const records = [];
        let position = offset;
        for (let family = range[0]; family < range[1]; ++family) {
            for (let index = 0; index < counts[family]; ++index) {
                records.push({ family, ...read(position) });
                position += size;
            }
        }
        return records;
    }

    function zeroPaddingProblem(view, from, to) {
        for (let offset = from; offset < to; ++offset) {
            if (view.getUint8(offset))
                return `padding byte ${offset} is ${view.getUint8(offset)}`;
        }
        return null;
    }

    // Section 3.2. Returns the parsed section with the format problems found on the way (V1, V5); a section too short for
    // its header or its arrays comes back with those problems and nothing else.
    function parseState(buffer) {
        const view = new DataView(buffer);
        const parsed = { byteLength: buffer.byteLength, problems: [] };
        if (buffer.byteLength < stateHeaderSize) {
            parsed.problems.push(`V1: ${buffer.byteLength} bytes hold no StateHeader`);
            return parsed;
        }
        const familyEntryCount = [];
        for (let family = 0; family < numberOfFamilies; ++family)
            familyEntryCount.push(view.getUint32(32 + 4 * family, true));
        const header = {
            layoutVersion: view.getUint16(0, true),
            tier: view.getUint8(2),
            counterMode: view.getUint8(3),
            numArguments: view.getUint32(4, true),
            numValueProfiles: view.getUint32(8, true),
            numLazyOperandProfiles: view.getUint32(12, true),
            optimizationDelayCounter: view.getUint16(16, true),
            reoptimizationRetryCounter: view.getUint16(18, true),
            counterValue: view.getInt32(20, true),
            counterTotalCount: view.getFloat32(24, true),
            counterActiveThreshold: view.getInt32(28, true),
            familyEntryCount,
            reserved1: view.getUint32(156, true),
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
        place("arrayProfiles", sum(familyEntryCount, arrayProfileFamilies), 8);
        place("lazyOperands", header.numLazyOperandProfiles, lazyOperandRecordSize);
        place("hints", sum(familyEntryCount, hintFamilies), 2);
        place("iterationModes", sum(familyEntryCount, iterationFamilies), 2);
        place("enumeratorModes", sum(familyEntryCount, enumeratorFamilies), 1);
        place("toThis", familyEntryCount[toThisFamilies[0]], 1);
        place("branchBits", familyEntryCount[branchFamilies[0]], 1);
        const size = roundUpToEight(end);
        if (buffer.byteLength !== size) {
            parsed.problems.push(`V5: the section has ${buffer.byteLength} bytes, and its header gives ${size}`);
            return parsed;
        }
        const padding = zeroPaddingProblem(view, end, size);
        if (padding)
            parsed.problems.push(`V5: ${padding}`);

        const predictions = (offset, count) => {
            const values = [];
            for (let index = 0; index < count; ++index)
                values.push(view.getBigUint64(offset + 8 * index, true));
            return values;
        };
        parsed.arguments = predictions(layout.arguments, header.numArguments);
        parsed.values = predictions(layout.values, header.numValueProfiles);
        parsed.arrayProfiles = readFamilies(familyEntryCount, arrayProfileFamilies, layout.arrayProfiles, 8, offset => ({
            modes: view.getUint32(offset, true),
            flags: view.getUint32(offset + 4, true),
        }));
        parsed.lazyOperands = [];
        for (let index = 0; index < header.numLazyOperandProfiles; ++index) {
            const offset = layout.lazyOperands + lazyOperandRecordSize * index;
            const bytecodeIndexBits = view.getUint32(offset, true);
            parsed.lazyOperands.push({
                bytecodeIndexBits,
                bytecodeOffset: bytecodeIndexBits >>> checkpointBits,
                checkpoint: bytecodeIndexBits & ((1 << checkpointBits) - 1),
                kind: view.getUint32(offset + 4, true),
                value: view.getInt32(offset + 8, true),
                reserved: view.getUint32(offset + 12, true),
                prediction: view.getBigUint64(offset + 16, true),
            });
        }
        parsed.hints = readFamilies(familyEntryCount, hintFamilies, layout.hints, 2, offset => {
            const bits = view.getUint16(offset, true);
            return { indexingType: bits >> 8, vectorLength: bits & 0xff };
        });
        parsed.iterationModes = readFamilies(familyEntryCount, iterationFamilies, layout.iterationModes, 2, offset => ({ modes: view.getUint16(offset, true) }));
        parsed.enumeratorModes = readFamilies(familyEntryCount, enumeratorFamilies, layout.enumeratorModes, 1, offset => ({ modes: view.getUint8(offset) }));
        parsed.toThis = readFamilies(familyEntryCount, toThisFamilies, layout.toThis, 1, offset => ({ status: view.getUint8(offset) }));
        parsed.branchBits = readFamilies(familyEntryCount, branchFamilies, layout.branchBits, 1, offset => ({ bit: view.getUint8(offset) }));
        return parsed;
    }

    // Section 3.3.
    function parseSummary(buffer) {
        const view = new DataView(buffer);
        const parsed = { byteLength: buffer.byteLength, problems: [] };
        if (buffer.byteLength < summaryHeaderSize) {
            parsed.problems.push(`summary V1: ${buffer.byteLength} bytes hold no SummaryHeader`);
            return parsed;
        }
        const header = {
            layoutVersion: view.getUint16(0, true),
            tier: view.getUint8(2),
            counterMode: view.getUint8(3),
            numArguments: view.getUint32(4, true),
            numValueProfiles: view.getUint32(8, true),
            numArrayProfiles: view.getUint32(12, true),
            numLazyOperandProfiles: view.getUint32(16, true),
            counterProgress: view.getUint32(20, true),
        };
        parsed.header = header;
        const categories = header.numArguments + header.numValueProfiles + header.numLazyOperandProfiles;
        const end = summaryHeaderSize + 8 * categories + header.numArrayProfiles;
        const size = roundUpToEight(end);
        if (buffer.byteLength !== size) {
            parsed.problems.push(`summary V5: the section has ${buffer.byteLength} bytes, and its header gives ${size}`);
            return parsed;
        }
        const padding = zeroPaddingProblem(view, end, size);
        if (padding)
            parsed.problems.push(`summary V5: ${padding}`);
        let offset = summaryHeaderSize;
        const take = count => {
            const values = [];
            for (let index = 0; index < count; ++index, offset += 8)
                values.push(view.getBigUint64(offset, true));
            return values;
        };
        parsed.arguments = take(header.numArguments);
        parsed.values = take(header.numValueProfiles);
        parsed.lazyOperands = take(header.numLazyOperandProfiles);
        parsed.arrayFlags = [];
        for (let index = 0; index < header.numArrayProfiles; ++index)
            parsed.arrayFlags.push(view.getUint8(offset + index));
        return parsed;
    }

    const hex = value => `0x${value.toString(16)}`;
    const outside = (prediction, mask) => (prediction & ~mask) !== 0n;

    // Section 4.3's counterProgress, from the counter fields of cb.state.
    function counterProgressOf(header) {
        if (header.counterMode !== CounterMode.Carried)
            return 0;
        const progress = header.counterTotalCount + header.counterValue;
        if (progress <= 0)
            return 0;
        return Math.min(Math.floor(progress), header.counterActiveThreshold);
    }

    // V1, V5 to V7, V8's type and vector-length rules, V9 to V12, V13 without its offset bound, V14, V15 and S3.
    function stateProblems(state) {
        const problems = [...state.problems];
        const header = state.header;
        if (!header || problems.length)
            return problems;
        if (header.layoutVersion !== 1)
            problems.push(`V1: layout version ${header.layoutVersion}`);
        if (header.tier !== 1)
            problems.push(`V1: tier ${header.tier}`);
        if (header.counterMode !== CounterMode.NotCarried && header.counterMode !== CounterMode.Carried)
            problems.push(`V1: counter mode ${header.counterMode}`);
        if (header.reserved1)
            problems.push(`V1: reserved1 is ${header.reserved1}`);
        state.arguments.forEach((prediction, index) => {
            if (outside(prediction, specBytecodeTop))
                problems.push(`V6: argument ${index} predicts ${hex(prediction)}`);
        });
        state.values.forEach((prediction, index) => {
            if (outside(prediction, specBytecodeTop))
                problems.push(`V6: value profile ${index + 1} predicts ${hex(prediction)}`);
        });
        state.arrayProfiles.forEach(record => {
            if (record.modes & ~allArrayModes)
                problems.push(`V7: ${familyNames[record.family]} has array modes ${hex(record.modes)}`);
            if (record.flags & ~arrayProfileFlagMask)
                problems.push(`V7: ${familyNames[record.family]} has flags ${hex(record.flags)}`);
        });
        state.hints.forEach(record => {
            const allowed = record.family === hintFamilies[1] - 1 ? copyOnWriteHints : writableArrayHints;
            if (!allowed.includes(record.indexingType))
                problems.push(`V8: ${familyNames[record.family]} hints indexing type ${hex(record.indexingType)}`);
            if (record.vectorLength > maximumVectorLengthHint)
                problems.push(`V8: ${familyNames[record.family]} hints vector length ${record.vectorLength}`);
        });
        state.iterationModes.forEach(record => {
            const allowed = nativeIterationModes[record.family - iterationFamilies[0]];
            if (record.modes & ~allowed)
                problems.push(`V9: ${familyNames[record.family]} holds modes ${hex(record.modes)}, outside ${hex(allowed)}`);
        });
        state.enumeratorModes.forEach(record => {
            if (record.modes & ~enumeratorModeMask)
                problems.push(`V10: ${familyNames[record.family]} holds ${hex(record.modes)}`);
        });
        state.toThis.forEach(record => {
            if (record.status > ToThisStatus.ClearedByGC)
                problems.push(`V11: to_this status ${record.status}`);
        });
        state.branchBits.forEach(record => {
            if (record.bit > 1)
                problems.push(`V12: branch bit ${record.bit}`);
        });
        state.lazyOperands.forEach((record, index) => {
            const where = `V13: lazy-operand record ${index}`;
            if (record.reserved)
                problems.push(`${where} has reserved ${record.reserved}`);
            if (outside(record.prediction, specBytecodeTop))
                problems.push(`${where} predicts ${hex(record.prediction)}`);
            if (record.bytecodeOffset >= unreachableOffset)
                problems.push(`${where} has bytecode index bits ${hex(record.bytecodeIndexBits)}`);
            switch (record.kind) {
            case OperandKind.Tmp:
                if (record.value < 0)
                    problems.push(`${where} names tmp ${record.value}`);
                break;
            case OperandKind.Argument:
                if (record.value < 0 || record.value === invalidVirtualRegister || record.value >= firstConstantRegisterIndex)
                    problems.push(`${where} names argument register ${record.value}`);
                break;
            case OperandKind.Local:
                if (record.value >= 0)
                    problems.push(`${where} names local register ${record.value}`);
                break;
            default:
                problems.push(`${where} has operand kind ${record.kind}`);
            }
        });
        if (header.optimizationDelayCounter > maximumOptimizationDelay)
            problems.push(`V14: optimization delay ${header.optimizationDelayCounter}`);
        if (header.reoptimizationRetryCounter > reoptimizationRetryCounterMax)
            problems.push(`V14: reoptimization count ${header.reoptimizationRetryCounter}`);
        if (header.counterMode === CounterMode.Carried) {
            if (header.counterActiveThreshold < 0)
                problems.push(`V15: threshold ${header.counterActiveThreshold}`);
            if (!Number.isFinite(header.counterTotalCount) || header.counterTotalCount < 0)
                problems.push(`V15: total count ${header.counterTotalCount}`);
            if (header.counterActiveThreshold !== int32Max && header.counterTotalCount + header.counterValue < 0)
                problems.push(`S3: progress ${header.counterTotalCount + header.counterValue} under the finite threshold ${header.counterActiveThreshold}`);
        } else if (header.counterValue || header.counterTotalCount || header.counterActiveThreshold) {
            problems.push(`V15: a NotCarried counter holds ${header.counterValue}, ${header.counterTotalCount}, ${header.counterActiveThreshold}`);
        }
        return problems;
    }

    // decodeSummary's rules (section 3.4), then I8 and I16 against the state of the same capture: equal counts and counter
    // mode, every slot a superset of its cb.state slot (equal for lazy operands, which have no UCB copy), every array flag
    // byte a superset of the CB's flags, and section 4.3's counterProgress.
    function summaryProblems(summary, state) {
        const problems = [...summary.problems];
        const header = summary.header;
        if (!header || problems.length)
            return problems;
        if (header.layoutVersion !== 1 || header.tier !== 1 || (header.counterMode !== CounterMode.NotCarried && header.counterMode !== CounterMode.Carried))
            problems.push(`summary V1: version ${header.layoutVersion}, tier ${header.tier}, counter mode ${header.counterMode}`);
        for (const [name, categories] of [["argument", summary.arguments], ["value", summary.values], ["lazy-operand", summary.lazyOperands]]) {
            categories.forEach((category, index) => {
                if (outside(category, specBytecodeTop))
                    problems.push(`summary V6: ${name} category ${index} is ${hex(category)}`);
            });
        }
        if (header.counterProgress > int32Max)
            problems.push(`summary: counterProgress ${header.counterProgress}`);
        if (header.counterMode === CounterMode.NotCarried && header.counterProgress)
            problems.push(`summary: a NotCarried summary has counterProgress ${header.counterProgress}`);
        if (!state.header || state.problems.length)
            return problems;

        const stateHeader = state.header;
        const arrayProfileCount = sum(stateHeader.familyEntryCount, arrayProfileFamilies);
        if (header.counterMode !== stateHeader.counterMode)
            problems.push(`I16: the summary's counter mode ${header.counterMode} differs from cb.state's ${stateHeader.counterMode}`);
        if (header.numArguments !== stateHeader.numArguments || header.numValueProfiles !== stateHeader.numValueProfiles
            || header.numLazyOperandProfiles !== stateHeader.numLazyOperandProfiles || header.numArrayProfiles !== arrayProfileCount) {
            problems.push("I8: the summary's counts differ from cb.state's");
            return problems;
        }
        const covers = (name, categories, predictions) => {
            predictions.forEach((prediction, index) => {
                if (prediction & ~categories[index])
                    problems.push(`I8: ${name} category ${index} ${hex(categories[index])} misses cb.state's ${hex(prediction)}`);
            });
        };
        covers("argument", summary.arguments, state.arguments);
        covers("value", summary.values, state.values);
        state.lazyOperands.forEach((record, index) => {
            if (summary.lazyOperands[index] !== record.prediction)
                problems.push(`I8: lazy-operand category ${index} ${hex(summary.lazyOperands[index])} is not cb.state's ${hex(record.prediction)}`);
        });
        state.arrayProfiles.forEach((record, index) => {
            if (record.flags & ~summary.arrayFlags[index])
                problems.push(`I8: array flags ${index} ${hex(summary.arrayFlags[index])} miss cb.state's ${hex(record.flags)}`);
        });
        const progress = counterProgressOf(stateHeader);
        if (header.counterProgress !== progress)
            problems.push(`I8: counterProgress ${header.counterProgress}, and cb.state's counter gives ${progress}`);
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

    // readBody, which must find a body whose sections pass every check above; throws otherwise.
    function checkBody(fn, kind, label) {
        const body = readBody(fn, kind);
        if (!body)
            throw new Error(`${label}: no committed body holds cb.state and cb.summary`);
        const problems = [...stateProblems(body.state), ...summaryProblems(body.summary, body.state)];
        if (problems.length)
            throw new Error(`${label}: ${problems.join("; ")}`);
        return body;
    }

    // Rewrites the counter of a committed body that carries one, `body` being what readBody or checkBody returned
    // (jitcacheRewriteSection): cb.state's counterValue and counterTotalCount, which sit back to back at offset 20, and
    // cb.summary's counterProgress at offset 20, which section 4.3 computes from them, so the body still passes I8.
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
    // LLInt and never compiled to baseline in this process (harness sub-SPEC section 10.1); throws otherwise.
    function expectImported(fn, kind, label) {
        const counts = globalThis.jitcacheBodyEvents(fn, kind);
        if (!counts || counts.llintInstructions || counts.baselineCompiles)
            throw new Error(`${label} was not imported at its first ${kind}: ${describe(counts)}`);
    }

    return Object.freeze({
        familyNames, CounterMode, OperandKind, IterationMode, EnumeratorMode, IndexingType, ToThisStatus,
        maxNumCheckpointTmps: 4,
        parseState, parseSummary, stateProblems, summaryProblems, counterProgressOf, entries,
        readBody, checkBody, rewriteCounter, asyncGeneratorBody, describe, expectImported,
    });
})();
