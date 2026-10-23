// The UCB lane's three sections, read in JavaScript so that twins-only tests can find the bytes they rewrite with
// jitcacheRewriteSection. Loaded with load("./resources/sections.js", "caller relative"); it defines one global,
// ucbSections, the same in every role.
//
// ucb.identity and ucb.feedback are read as the lane writes them. A function's ucb.core is read as
// runtime/CachedTypes.cpp lays it out: the 16-byte header, the CachedFunctionCodeBlock record, whose one member is the
// 4-byte CachedPtr of its expression info, and the record's tail of varints (packLayout), which locates the instruction
// stream and the constant pool. Every reader checks what it relies on and throws with the bytes it found otherwise, so a
// layout change shows up as a clear failure rather than as a rewrite of the wrong bytes.
var ucbSections = (function () {
    "use strict";

    // The shared byte reader. Its file defines no global, so ucbSections stays the one global this file adds.
    const { byteReader } = load("../../resources/jitcache.js", "caller relative");

    function hex(bytes)
    {
        let text = "";
        for (const byte of bytes)
            text += byte.toString(16).padStart(2, "0");
        return text;
    }

    function ascii(text)
    {
        const bytes = new Uint8Array(text.length);
        for (let i = 0; i < text.length; ++i)
            bytes[i] = text.charCodeAt(i);
        return bytes;
    }

    function le32(value)
    {
        return [value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff];
    }

    function align(value, alignment)
    {
        return Math.ceil(value / alignment) * alignment;
    }

    function bit(bytes, start, index)
    {
        return (bytes[start + (index >> 3)] >> (index & 7)) & 1;
    }

    function find(bytes, pattern, from = 0)
    {
        outer: for (let i = from; i + pattern.length <= bytes.length; ++i) {
            for (let j = 0; j < pattern.length; ++j) {
                if (pattern[j] !== null && bytes[i + j] !== pattern[j])
                    continue outer;
            }
            return i;
        }
        return -1;
    }

    function failure(what, bytes)
    {
        return new Error(`${what}; ${bytes.length} bytes: ${hex(bytes.subarray(0, Math.min(bytes.length, 256)))}`);
    }

    // ucb.identity.
    function identity(bytes)
    {
        const read = byteReader(bytes);
        const constantCount = read.u32(108);
        const section = {
            bytes,
            key: hex(bytes.subarray(0, 40)),
            holderDigest: hex(bytes.subarray(72, 104)),
            features: read.u16(104),
            lexicallyScopedFeatures: bytes[106],
            hasCapturedVariables: bytes[107],
            constantCount,
            coreKind: bytes[112],
            butterflyMap: 113,
            end: 113 + Math.ceil(constantCount / 8),
        };
        section.butterfly = index => bit(bytes, section.butterflyMap, index);
        // The child holder digests, 32 bytes each, follow the map's padding: declarations, then expressions.
        section.childHolderDigests = align(section.end, 8);
        section.childHolderDigestCount = (bytes.length - section.childHolderDigests) / 32;
        section.childHolderDigest = index => hex(bytes.subarray(section.childHolderDigests + 32 * index, section.childHolderDigests + 32 * (index + 1)));
        if (!Number.isInteger(section.childHolderDigestCount) || section.childHolderDigestCount < 0)
            throw failure("ucb.identity does not have the lane's identity layout", bytes);
        return section;
    }

    // ucb.feedback, with the LLInt counter its header carries: the active threshold, and the progress, the float total
    // count plus the int32 counter, as FeedbackSection::llintProgress adds them. The UCB's value predictions and array
    // profiles are there only when the copies byte, byte 31, is 1; V and A count the UCB's profiles either way.
    function feedback(bytes)
    {
        const read = byteReader(bytes);
        const counts = {
            valueProfiles: read.u32(0),
            arrayProfiles: read.u32(4),
            binaryArithProfiles: read.u32(8),
            unaryArithProfiles: read.u32(12),
            exitSites: read.u32(16),
            functionDecls: read.u32(20),
            functionExprs: read.u32(24),
            constants: read.u32(44),
        };
        const copies = bytes[31];
        const predictions = 48;
        const arrays = predictions + (copies ? 8 * counts.valueProfiles : 0);
        const binary = arrays + (copies ? 8 * counts.arrayProfiles : 0);
        const unary = binary + 2 * counts.binaryArithProfiles;
        const exits = align(unary + 2 * counts.unaryArithProfiles, 4);
        const children = exits + 8 * counts.exitSites;
        const constants = children + counts.functionDecls + counts.functionExprs;
        const end = constants + Math.ceil(counts.constants / 8);
        const section = { bytes, counts, copies, predictions, arrays, binary, unary, exits, children, constants, end };
        if (copies > 1 || align(end, 8) !== bytes.length)
            throw failure("ucb.feedback does not have the lane's feedback layout", bytes);
        section.llintActiveThreshold = read.i32(32);
        section.llintProgress = read.f32(36) + read.i32(40);
        return section;
    }

    function readVarint(bytes, cursor)
    {
        const start = cursor.at;
        let value = 0;
        let shift = 0;
        for (;;) {
            const byte = bytes[cursor.at++];
            value += (byte & 0x7f) * 2 ** shift;
            if (!(byte & 0x80))
                break;
            shift += 7;
            if (shift > 35)
                throw failure(`a varint at ${start} runs on`, bytes);
        }
        return { value, offset: start, length: cursor.at - start };
    }

    const zigzag = encoded => (encoded % 2 ? -(encoded + 1) / 2 : encoded / 2);

    // The number of CachedJSValue::Kind values runtime/CachedTypes.cpp defines.
    const kindCount = 14;

    // A function's ucb.core (CachedCodeBlock::packLayout in runtime/CachedTypes.cpp).
    function functionCore(bytes)
    {
        const read = byteReader(bytes);
        if (read.u32(0) !== 0x43424355 || bytes[4] !== 3)
            throw failure("ucb.core is no function core", bytes);
        const rootOffset = read.u32(8);
        // The record holds only the CachedPtr of its expression info; its tail of varints follows it.
        const recordSize = 4;
        const cursor = { at: rootOffset + recordSize };
        const flags = bytes[cursor.at++];
        const recordOffsetInRegion = readVarint(bytes, cursor);
        if (flags & 1)
            readVarint(bytes, cursor); // the metadata's value profile count
        const array = () => {
            const count = readVarint(bytes, cursor);
            const at = count.value ? readVarint(bytes, cursor) : null;
            return { count, at: at ? zigzag(at.value) : 0 };
        };
        const steps = array();
        const instructions = array();
        const constants = array();
        const representations = array();
        const regionBegin = rootOffset - recordOffsetInRegion.value;

        const core = { bytes, rootOffset, flags, regionBegin, steps, instructions, constants, representations };
        core.instructionsStart = regionBegin + instructions.at;
        core.poolStart = regionBegin + constants.at;
        core.constantCount = constants.count.value;
        core.representationsStart = regionBegin + representations.at;
        core.kind = index => bytes[core.poolStart + index];

        const inside = (start, length) => start >= 16 && start + length <= bytes.length;
        let consistent = regionBegin >= 16 && regionBegin <= rootOffset
            && inside(core.instructionsStart, instructions.count.value)
            && (!core.constantCount || inside(core.poolStart, align(core.constantCount, 4) + 4 * core.constantCount))
            && (!representations.count.value || inside(core.representationsStart, representations.count.value));
        for (let i = 0; consistent && i < core.constantCount; ++i)
            consistent = core.kind(i) < kindCount;
        // The expression info is reached through the record's CachedPtr: a 4-aligned offset from the slot itself.
        const expressionInfo = read.i32(rootOffset);
        consistent = consistent && !(expressionInfo & 3) && inside(rootOffset + expressionInfo, 4);
        if (!consistent)
            throw failure(`ucb.core does not have the layout this test reads: ${JSON.stringify({ rootOffset, flags, regionBegin, instructions: instructions.count.value, constants: core.constantCount, expressionInfo })}`, bytes);
        return core;
    }

    return {
        hex, ascii, le32, align, bit, find,
        identity, feedback, functionCore,
    };
})();
