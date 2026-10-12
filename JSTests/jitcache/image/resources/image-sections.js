// Readers of the Image lane's sections for the scripts that compare captures (SPEC-image.md sections 8.2, 8.3 and 11.2),
// the check a script makes that its imports happened (HARNESS.md, Tests), and the check that its images carry the
// targets its header claims, which together cover table 3.3 and the census (T1). A script loads this file inside its main
// function, const sections = load("./resources/image-sections.js", "caller relative"), which returns the object at the end
// of this file and defines no global, so concurrency.js can load several corpus scripts that each load it. Every field is
// little-endian and every region starts 8-byte aligned. The image.baseline reader walks the regions in the order
// ImageSectionsView::locate does and returns each region's bytes beside the entries it decodes, so the scripts compare
// captures region by region. Only twins builds register the shell functions the readers and checks call (harness
// sub-SPEC section 5.2), and the pin has none of them, so scripts call the readers and checks only in twins runs and
// never in the Off role, which the oracle and the pin comparison run. A script whose Off run calls one of those functions
// itself, as a wait that polls jitcacheBodyEvents until a CodeBlock ages out does, declares jitcache-pin: off with its
// reason (SPEC-integrator.md R-ALL-4).
(function () {
    "use strict";

    const imageFixupForm = Object.freeze({ Pointer: 1, Call: 2, Jump: 3 });

    // Table 3.3's order, which the section's kind byte numbers from 1.
    const imageTargetKind = Object.freeze({
        Operation: 1, CommonThunk: 2, BaselineThunk: 3, SlowPathThunk: 4, InlineCacheSlowPathThunk: 5, VirtualCallThunk: 6,
        ProcessThunk: 7, VMAddress: 8, VMCell: 9, StructureIDBase: 10, UCBConstantCell: 11, UCBConstantAtom: 12,
        UCBIdentifier: 13, UCBBinaryArithProfile: 14, UCBUnaryArithProfile: 15, SwitchStringRankAtom: 16,
        SwitchStringRankCase: 17, MathIC: 18, SwitchTableBase: 19, ImageOffset: 20, SnippetEntry: 21,
    });

    // The a field of the kinds table 3.3 keys by an enumeration (sections 3.4 and 3.6). BaselineThunk, ProcessThunk,
    // VMAddress and VMCell are the lane's own and number their entries from 1. CommonJITThunkID and CallMode are the
    // engine's, stored as their native values, which the header's build ID fixes (section 8.1): CommonJITThunkID counts
    // the entries of JSC_FOR_EACH_COMMON_THUNK (jit/JITThunks.h) from 0, and CallMode those of bytecode/CallMode.h.
    const commonJITThunkID = Object.freeze({ ReturnFromBaseline: 4, HandleException: 63, ThrowStackOverflowAtPrologue: 73 });
    const callMode = Object.freeze({ Regular: 0, Tail: 1, Construct: 2 });
    const baselineThunk = Object.freeze({
        OpEnterHandler: 1, OpCheckTrapsHandler: 2, OpThrowHandler: 3, ValueIsTruthy: 4, ValueIsFalsey: 5, SlowOpPutToScope: 6,
        ResolveScopeClosureVarWithVarInjectionChecks: 7, ResolveScopeGlobalVar: 8, ResolveScopeGlobalProperty: 9,
        ResolveScopeGlobalLexicalVar: 10, ResolveScopeGlobalVarWithVarInjectionChecks: 11,
        ResolveScopeGlobalPropertyWithVarInjectionChecks: 12, ResolveScopeGlobalLexicalVarWithVarInjectionChecks: 13,
        GetFromScopeGlobalVar: 14, GetFromScopeGlobalProperty: 15, GetFromScopeGlobalLexicalVar: 16,
        GetFromScopeGlobalVarWithVarInjectionChecks: 17, GetFromScopeGlobalLexicalVarWithVarInjectionChecks: 18,
        ConsistencyCheck: 19,
    });
    const processThunk = Object.freeze({ DefaultCall: 1, ArityFixup: 2 });
    const vmAddress = Object.freeze({
        VM: 1, SoftStackLimit: 2, TrapBits: 3, Exception: 4, TopEntryFrame: 5, TargetMachinePCForThrow: 6, TopCallFrame: 7,
        MightBeExecutingTaintedCode: 8, BarrierThreshold: 9, MutatorShouldBeFenced: 10, SyncResumeCallCache: 11,
    });
    const vmCell = Object.freeze({ EmptyString: 1, SmallStringsSentinel: 2 });
    const targetFieldA = Object.freeze({
        CommonThunk: commonJITThunkID, BaselineThunk: baselineThunk, VirtualCallThunk: callMode, ProcessThunk: processThunk,
        VMAddress: vmAddress, VMCell: vmCell,
    });

    const imageMathICFlags = Object.freeze({ generateFastPathOnRepatch: 1, hasSnippet: 2, noInlineCode: 4 });

    // MathICKind, the kind byte of region 9.
    const imageMathICKind = Object.freeze({ Add: 1, Sub: 2, Mul: 3, Negate: 4 });

    // CompileInputKind and StrictEqualityAtomOperand (section 11.1), the kind and value bytes of the twins section's compile
    // inputs.
    const twinCompileInputKind = Object.freeze({
        ResolveScopeType: 1, GetFromScopeType: 2, PutToScopeType: 3, GetByIdMode: 4, IteratorOpenMode: 5,
        AsyncIteratorOpenMode: 6, EnumeratorMetadata: 7, StrictEqualityAtomOperand: 8,
    });
    const strictEqualityAtomOperand = Object.freeze({ None: 0, Lhs: 1, Rhs: 2 });

    // The sections of a key's current body, each an ArrayBuffer or null.
    function readBodySections(key) {
        return {
            image: jitcacheReadSection(key, "image.baseline"),
            bakedFacts: jitcacheReadSection(key, "baked-facts.baseline"),
            twins: jitcacheReadSection(key, "image-twins.baseline"),
            cbState: jitcacheReadSection(key, "cb.state"),
            cbSummary: jitcacheReadSection(key, "cb.summary"),
        };
    }

    function imageSectionAlign(offset) {
        return (offset + 7) & ~7;
    }

    function sameImageBytes(a, b) {
        const x = a instanceof Uint8Array ? a : new Uint8Array(a);
        const y = b instanceof Uint8Array ? b : new Uint8Array(b);
        if (x.length !== y.length)
            return false;
        for (let i = 0; i < x.length; ++i) {
            if (x[i] !== y[i])
                return false;
        }
        return true;
    }

    function sameImageFixup(x, y) {
        return x.site === y.site && x.form === y.form && x.kind === y.kind && x.a === y.a && x.b === y.b && x.payload === y.payload;
    }

    function describeImageFixup(fixup) {
        return `{site ${fixup.site}, form ${fixup.form}, kind ${fixup.kind}, a ${fixup.a}, b ${fixup.b}, payload ${fixup.payload}}`;
    }

    function readImageFixups(view, offset, count) {
        const fixups = [];
        for (let i = 0; i < count; ++i, offset += 24) {
            fixups.push({
                site: view.getUint32(offset, true),
                form: view.getUint8(offset + 4),
                kind: view.getUint8(offset + 5),
                a: view.getUint32(offset + 8, true),
                b: view.getUint32(offset + 12, true),
                payload: view.getBigInt64(offset + 16, true),
            });
        }
        return fixups;
    }

    // image.baseline (section 8.2): the 56-byte header, then code, fixups, calls, molds, simple and string switch tables,
    // code map, constant pool, MathICs and the snippets of the MathICs whose flags say they have one.
    function parseImageSection(buffer) {
        const view = new DataView(buffer);
        const u32 = offset => view.getUint32(offset, true);
        const header = {
            codeSize: u32(0),
            arityEntryOffset: u32(4),
            fixupCount: u32(8),
            callCount: u32(12),
            moldCount: u32(16),
            simpleSwitchTableCount: u32(20),
            stringSwitchTableCount: u32(24),
            codeMapCount: u32(28),
            constantPoolCount: u32(32),
            mathICCount: u32(36),
            livenessRateBits: view.getBigUint64(40, true),
            fullnessRateBits: view.getBigUint64(48, true),
        };
        let offset = 56;
        const region = length => {
            const bytes = new Uint8Array(buffer, offset, length);
            offset += length;
            return bytes;
        };

        const code = region(header.codeSize);
        offset = imageSectionAlign(offset);
        const fixups = readImageFixups(view, offset, header.fixupCount);
        offset += 24 * header.fixupCount;
        const calls = region(8 * header.callCount);
        const molds = region(16 * header.moldCount);

        const simpleStart = offset;
        for (let i = 0; i < header.simpleSwitchTableCount; ++i)
            offset += 8 + 4 * u32(offset + 4);
        const simpleTables = new Uint8Array(buffer, simpleStart, offset - simpleStart);
        offset = imageSectionAlign(offset);

        const stringStart = offset;
        for (let i = 0; i < header.stringSwitchTableCount; ++i)
            offset += 4 + 4 * u32(offset);
        const stringTables = new Uint8Array(buffer, stringStart, offset - stringStart);
        offset = imageSectionAlign(offset);

        const codeMap = region(4 * header.codeMapCount);
        offset = imageSectionAlign(offset);
        const constantPool = region(8 * header.constantPoolCount);

        const mathICs = [];
        for (let index = 0; index < header.mathICCount; ++index) {
            const at = offset + 32 * index;
            mathICs.push({
                index,
                kind: view.getUint8(at),
                flags: view.getUint8(at + 1),
                bytecodeOffset: u32(at + 4),
                inlineStart: u32(at + 8),
                inlineEnd: u32(at + 12),
                slowPathStart: u32(at + 16),
                slowPathCall: u32(at + 20),
                snippetSize: u32(at + 24),
                snippetFixupCount: u32(at + 28),
                entry: new Uint8Array(buffer, at, 32),
                snippet: null,
            });
        }
        offset += 32 * header.mathICCount;
        for (const mathIC of mathICs) {
            if (!(mathIC.flags & imageMathICFlags.hasSnippet))
                continue;
            const snippetCode = region(mathIC.snippetSize);
            offset = imageSectionAlign(offset);
            const snippetFixups = readImageFixups(view, offset, mathIC.snippetFixupCount);
            offset += 24 * mathIC.snippetFixupCount;
            mathIC.snippet = { code: snippetCode, fixups: snippetFixups };
        }

        if (offset !== buffer.byteLength)
            throw new Error(`image.baseline holds ${buffer.byteLength} bytes where its counts imply ${offset}`);
        return { header, code, fixups, calls, molds, simpleTables, stringTables, codeMap, constantPool, mathICs };
    }

    // A fixup's footprint [start, end) in its code (table 3.2). An x86_64 Jump is five bytes after E9 and six after 0F 8x.
    function imageFootprint(fixup, code, arm64) {
        const site = fixup.site;
        if (arm64) {
            switch (fixup.form) {
            case imageFixupForm.Pointer:
                return [site, site + 12];
            case imageFixupForm.Call:
                return [site - 4, site];
            default:
                return [site, site + 4];
            }
        }
        switch (fixup.form) {
        case imageFixupForm.Pointer:
            return [site - 10, site];
        case imageFixupForm.Call:
            return [site - 5, site];
        default:
            return code[site - 5] === 0xE9 ? [site - 5, site] : [site - 6, site];
        }
    }

    // image-twins.baseline (section 11.2): the 48-byte header, then the BinarySwitch seeds, the compile inputs, the binary
    // and unary arithmetic profile bits, the regeneration log and the producer values. Returns the header, the compile
    // inputs and the log, each log entry with its assembler seeds in attach order, null where the attach's assembler drew
    // none.
    function parseTwinsSection(buffer) {
        const view = new DataView(buffer);
        const u32 = offset => view.getUint32(offset, true);
        const header = {
            flags: view.getUint8(0),
            assemblerSeed: u32(4),
            binarySwitchSeedCount: u32(8),
            inputCount: u32(12),
            binaryArithProfileCount: u32(16),
            unaryArithProfileCount: u32(20),
            regenerationCount: u32(24),
            producerValueCount: u32(28),
        };
        let offset = imageSectionAlign(48 + 4 * header.binarySwitchSeedCount);

        // Each input: u32 bytecode offset, u8 kind, u8 value, u16 reserved, u32 local scope depth.
        const inputs = [];
        for (let i = 0; i < header.inputCount; ++i, offset += 12)
            inputs.push({ bytecodeOffset: u32(offset), kind: view.getUint8(offset + 4), value: view.getUint8(offset + 5), localScopeDepth: u32(offset + 8) });
        offset = imageSectionAlign(offset);
        offset = imageSectionAlign(offset + 2 * header.binaryArithProfileCount);
        offset = imageSectionAlign(offset + 2 * header.unaryArithProfileCount);

        // Each entry: u32 MathIC index, u16 profile bits at entry, u8 attach count, u8 seed mask, i64 replacement, then one
        // u32 seed per attach, padded to 8.
        const regenerations = [];
        for (let i = 0; i < header.regenerationCount; ++i) {
            const attachCount = view.getUint8(offset + 6);
            const seedMask = view.getUint8(offset + 7);
            const seeds = [];
            for (let slot = 0; slot < attachCount; ++slot)
                seeds.push(seedMask & (1 << slot) ? u32(offset + 16 + 4 * slot) : null);
            regenerations.push({
                mathICIndex: u32(offset),
                profileBitsAtEntry: view.getUint16(offset + 4, true),
                replacement: view.getBigInt64(offset + 8, true),
                seeds,
            });
            offset += imageSectionAlign(16 + 4 * attachCount);
        }
        offset += 8 * header.producerValueCount;

        if (offset !== buffer.byteLength)
            throw new Error(`image-twins.baseline holds ${buffer.byteLength} bytes where its counts imply ${offset}`);
        return { header, inputs, regenerations };
    }

    // image-twins.baseline (section 11.2): equal except the capture-process token, header bytes 32 to 48, and the producer
    // values, the region that ends the section, which hold the capturing process's own token and resolutions.
    function sameTwinsExceptCapturingProcess(a, b) {
        const x = new Uint8Array(a);
        const y = new Uint8Array(b);
        const countA = new DataView(a).getUint32(28, true);
        const countB = new DataView(b).getUint32(28, true);
        if (x.length !== y.length || countA !== countB)
            return false;
        const producerValues = x.length - 8 * countA;
        for (let i = 0; i < producerValues; ++i) {
            if (i >= 32 && i < 48)
                continue;
            if (x[i] !== y[i])
                return false;
        }
        return true;
    }

    // Whether this run determines which bodies it imports, so that a script checks its imports: a Consumer or a
    // ConsumerProducer of a twins build, which alone has jitcacheBodyEvents, with concurrent JIT off. With it on, as in
    // concurrency.js (T14), a producer can exit before it finalizes a plan it enqueued, so which bodies it committed
    // depends on the schedule.
    function checksImports(role) {
        return (role === "Consumer" || role === "ConsumerProducer") && typeof jitcacheBodyEvents === "function" && !jscOptions().useConcurrentJIT;
    }

    // fn's CodeBlock of kind installed an imported body when it was created, so its UCB began no LLInt instruction and
    // compiled no baseline code in this process (harness sub-SPEC section 10.1); throws otherwise. A body the producer
    // never compiled has no file to import, so a script checks only bodies its producer runs past the LLInt's tier-up.
    function expectImported(fn, kind = "call", label = fn.name) {
        const counts = jitcacheBodyEvents(fn, kind);
        if (!counts || counts.llintInstructions || counts.baselineCompiles)
            throw new Error(`${label} was not imported at its first ${kind}: ${JSON.stringify(counts)}`);
    }

    // The check a corpus script makes at its end: in a run that determines its imports (checksImports), every body of
    // bodies, an object from a label to a function, ran its import at its first use of kind (expectImported); in any
    // other run it does nothing. The labels name the bodies in errors, so the check reads no function's name, which would
    // reify it.
    function expectImports(role, bodies, kind = "call") {
        if (!checksImports(role))
            return;
        for (const [label, fn] of Object.entries(bodies))
            expectImported(fn, kind, label);
    }

    // Whether a parsed image.baseline holds ARM64 code, read from the canonical footprint of its first Pointer fixup
    // (section 3.2): on x86_64 a movabs whose eight immediate bytes end at the site, on ARM64 a movz Xd, #0 that starts
    // there. Every image has one, since its prologue compares the soft stack limit (census B1).
    function isArm64Image(image) {
        const code = image.code;
        const pointer = image.fixups.find(fixup => fixup.form === imageFixupForm.Pointer);
        if (!pointer)
            throw new Error("an image without a Pointer fixup cannot show its architecture");
        const site = pointer.site;
        if (site >= 10 && (code[site - 10] === 0x48 || code[site - 10] === 0x49) && (code[site - 9] & 0xF8) === 0xB8
            && code.subarray(site - 8, site).every(byte => !byte))
            return false;
        if (site + 4 <= code.length && code[site] < 32 && !code[site + 1] && code[site + 2] === 0x80 && code[site + 3] === 0xD2)
            return true;
        throw new Error(`the Pointer fixup at ${site} holds neither architecture's canonical footprint`);
    }

    // The body function a generator or async generator object resumes, which JSGenerator and JSAsyncGenerator keep in
    // internal field 1 (Field::Next), where only builtin-mode code reads. $vm exists in every twins run.
    let generatorBodyReader = null;
    function generatorBody(generator) {
        generatorBodyReader ??= $vm.createBuiltin("(function (generator) { return @getInternalField(generator, 1); })");
        return generatorBodyReader(generator);
    }

    // The fixups of a parsed image.baseline whose footprints lie in the slow call of one of its MathICs with inline code,
    // from the slow path's start to the call's return point (region 9's slowPathStart and slowPathCall). There
    // JIT::emitMathICSlow emits only the call's arguments and its far call, so the references there are the call's
    // Operation, in debug builds the top call frame, and the one operand that names the MathIC (census F3) or its
    // arithmetic profile (F4). A profile there is therefore F4's, while anywhere else it can be a profile write (A14) or the
    // operand of a site without inline code (F2), which have the same target.
    function mathICSlowCallFixups(image, arm64) {
        const slowCalls = image.mathICs.filter(mathIC => !(mathIC.flags & imageMathICFlags.noInlineCode));
        return image.fixups.filter(fixup => {
            const [start, end] = imageFootprint(fixup, image.code, arm64);
            return slowCalls.some(mathIC => mathIC.slowPathStart <= start && end <= mathIC.slowPathCall);
        });
    }

    // What fn's body of kind carries in the artifact the run reads: the fixups of its image and of every snippet, those
    // of its MathICs' slow calls (mathICSlowCallFixups), the access types its molds list (region 4, the first byte of
    // each 16-byte entry) and its architecture.
    function bodyTargets(fn, kind, label) {
        const key = jitcacheBodyKey(fn, kind);
        const bytes = key ? jitcacheReadSection(key, "image.baseline") : null;
        if (!bytes)
            throw new Error(`${label}'s key ${key} holds no image.baseline`);
        const image = parseImageSection(bytes);
        const fixups = [...image.fixups];
        for (const mathIC of image.mathICs) {
            if (!mathIC.snippet)
                continue;
            for (const fixup of mathIC.snippet.fixups)
                fixups.push(fixup);
        }
        const accessTypes = new Set();
        for (let offset = 0; offset < image.molds.length; offset += 16)
            accessTypes.add(image.molds[offset]);
        const arm64 = isArm64Image(image);
        return { fixups, mathICSlowCall: mathICSlowCallFixups(image, arm64), accessTypes, arm64 };
    }

    // T1's check of a script's claims, in a run that determines its imports (checksImports): the images of bodies, an
    // object from a kind ("call" or "construct") to an object from a label to a function, together carry a fixup of each
    // target claims names. A claim gives the census site or section it comes from, its kind and form by their names in
    // imageTargetKind and imageFixupForm, and its a by name where table 3.3 keys a by an enumeration (targetFieldA); a
    // kind whose a is an index into the body's own tables is claimed without one. A claim with eachMoldAccessType holds
    // one fixup for each access type the bodies' molds list, and a claim with arch "arm64" holds for ARM64 code only. A
    // claim with where "mathICSlowCall" holds only a fixup that lies in a MathIC's slow call (mathICSlowCallFixups), for a
    // site whose target other sites also emit elsewhere. Throws naming every claim no image backs.
    function expectTargets(role, bodies, claims) {
        if (!checksImports(role))
            return;
        const fixups = [];
        const placed = { mathICSlowCall: [] };
        const accessTypes = new Set();
        let arm64 = false;
        for (const [kind, labeled] of Object.entries(bodies)) {
            for (const [label, fn] of Object.entries(labeled)) {
                const targets = bodyTargets(fn, kind, label);
                for (const fixup of targets.fixups)
                    fixups.push(fixup);
                for (const fixup of targets.mathICSlowCall)
                    placed.mathICSlowCall.push(fixup);
                targets.accessTypes.forEach(accessType => accessTypes.add(accessType));
                arm64 = targets.arm64;
            }
        }

        const unbacked = [];
        for (const claim of claims) {
            if (claim.arch === "arm64" && !arm64)
                continue;
            const kind = imageTargetKind[claim.kind];
            const form = imageFixupForm[claim.form];
            const a = claim.a === undefined ? undefined : targetFieldA[claim.kind]?.[claim.a];
            const pool = claim.where === undefined ? fixups : Object.hasOwn(placed, claim.where) ? placed[claim.where] : undefined;
            if (kind === undefined || form === undefined || (claim.a !== undefined && a === undefined) || !pool)
                throw new Error(`malformed claim ${JSON.stringify(claim)}`);
            const backed = value => pool.some(fixup => fixup.kind === kind && fixup.form === form && (value === undefined || fixup.a === value));
            if (claim.eachMoldAccessType) {
                if (!accessTypes.size)
                    unbacked.push(`${claim.site}: no mold to take an access type from`);
                for (const accessType of accessTypes) {
                    if (!backed(accessType))
                        unbacked.push(`${claim.site}: ${claim.kind} ${claim.form} for access type ${accessType}`);
                }
                continue;
            }
            if (!backed(a))
                unbacked.push(`${claim.site}: ${claim.kind}${claim.a === undefined ? "" : ` ${claim.a}`} ${claim.form}${claim.where === undefined ? "" : ` in a ${claim.where}`}`);
        }
        if (unbacked.length)
            throw new Error(`no image of ${Object.values(bodies).flatMap(Object.keys).join(", ")} backs ${unbacked.join("; ")}`);
    }

    return Object.freeze({
        imageFixupForm, imageTargetKind, imageMathICFlags, imageMathICKind, twinCompileInputKind, strictEqualityAtomOperand,
        commonJITThunkID, callMode, baselineThunk, processThunk, vmAddress, vmCell,
        readBodySections, sameImageBytes, sameImageFixup, describeImageFixup, parseImageSection, imageFootprint,
        parseTwinsSection, sameTwinsExceptCapturingProcess, checksImports, expectImported, expectImports, isArm64Image,
        generatorBody, bodyTargets, expectTargets,
    });
})();
