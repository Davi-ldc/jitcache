// The UCB lane's three sections, read in JavaScript so that twins-only tests can find the bytes they rewrite with
// jitcacheRewriteSection (SPEC-integrator.harness.md section 5.2). Loaded with load("./resources/sections.js",
// "caller relative"); it defines one global, ucbSections, the same in every role.
//
// ucb.identity and ucb.feedback follow SPEC-ucb.md sections 4.2 and 5.2. A function's ucb.core is read as
// runtime/CachedTypes.cpp lays it out (SPEC-ucb.codec.md section 3): the 16-byte header, the CachedFunctionCodeBlock
// record, whose one member is the 4-byte CachedPtr of its expression info, and the record's tail of varints (packLayout),
// which locates the instruction stream and the constant pool. Every reader checks what it relies on and throws with the
// bytes it found otherwise, so a layout change shows up as a clear failure rather than as a rewrite of the wrong bytes.
var ucbSections = (function () {
    "use strict";

    const roundConstants = new Uint32Array([
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]);

    function rotateRight(value, count)
    {
        return (value >>> count) | (value << (32 - count));
    }

    // FIPS 180-4 SHA-256 of a Uint8Array.
    function sha256(message)
    {
        const blockCount = Math.ceil((message.length + 9) / 64);
        const padded = new Uint8Array(blockCount * 64);
        padded.set(message);
        padded[message.length] = 0x80;
        const view = new DataView(padded.buffer);
        const bitLength = message.length * 8;
        view.setUint32(padded.length - 8, Math.floor(bitLength / 0x100000000));
        view.setUint32(padded.length - 4, bitLength >>> 0);

        const state = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
        const schedule = new Uint32Array(64);
        for (let block = 0; block < blockCount; ++block) {
            for (let i = 0; i < 16; ++i)
                schedule[i] = view.getUint32(block * 64 + i * 4);
            for (let i = 16; i < 64; ++i) {
                const s0 = rotateRight(schedule[i - 15], 7) ^ rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >>> 3);
                const s1 = rotateRight(schedule[i - 2], 17) ^ rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >>> 10);
                schedule[i] = (schedule[i - 16] + s0 + schedule[i - 7] + s1) >>> 0;
            }
            let [a, b, c, d, e, f, g, h] = state;
            for (let i = 0; i < 64; ++i) {
                const s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
                const choice = (e & f) ^ (~e & g);
                const t1 = (h + s1 + choice + roundConstants[i] + schedule[i]) >>> 0;
                const s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
                const majority = (a & b) ^ (a & c) ^ (b & c);
                const t2 = (s0 + majority) >>> 0;
                h = g;
                g = f;
                f = e;
                e = (d + t1) >>> 0;
                d = c;
                c = b;
                b = a;
                a = (t1 + t2) >>> 0;
            }
            state[0] = (state[0] + a) >>> 0;
            state[1] = (state[1] + b) >>> 0;
            state[2] = (state[2] + c) >>> 0;
            state[3] = (state[3] + d) >>> 0;
            state[4] = (state[4] + e) >>> 0;
            state[5] = (state[5] + f) >>> 0;
            state[6] = (state[6] + g) >>> 0;
            state[7] = (state[7] + h) >>> 0;
        }
        const digest = new Uint8Array(32);
        const digestView = new DataView(digest.buffer);
        for (let i = 0; i < 8; ++i)
            digestView.setUint32(i * 4, state[i]);
        return digest;
    }

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

    // SPEC-ucb.md section 3.5: one encoding byte, 1 for Latin-1 when every unit is at most 0xFF and 2 for UTF-16LE
    // otherwise, then the code units.
    function sourceDigest(text)
    {
        let latin1 = true;
        for (let i = 0; i < text.length; ++i) {
            if (text.charCodeAt(i) > 0xff) {
                latin1 = false;
                break;
            }
        }
        const unitSize = latin1 ? 1 : 2;
        const bytes = new Uint8Array(1 + text.length * unitSize);
        bytes[0] = latin1 ? 1 : 2;
        for (let i = 0; i < text.length; ++i) {
            const unit = text.charCodeAt(i);
            if (latin1)
                bytes[1 + i] = unit;
            else {
                bytes[1 + 2 * i] = unit & 0xff;
                bytes[2 + 2 * i] = unit >> 8;
            }
        }
        return sha256(bytes);
    }

    // The key of a program body whose source is the whole of `text`, as the jsc shell's loadString evaluates it: kind
    // Program, call, the empty code-generation mode, and the root identity record of SPEC-ucb.md section 3.2 with the
    // snapshot's strict and with-scope bits clear (a ProgramExecutable starts with no features, F18). The shell's
    // providers supply no digest, so the root's source digest is computed from the text (section 3.5, form 3).
    function programKeyHex(text)
    {
        const record = new Uint8Array(56);
        record.set(ascii("JITCache.root.v1"), 0);
        record[16] = 1; // IdentityKind::Program
        record.set(sourceDigest(text), 24);
        const key = new Uint8Array(40);
        key[0] = 1; // version
        key[1] = 1; // IdentityKind::Program
        key.set(sha256(record), 8);
        return hex(key);
    }

    function u16(bytes, offset)
    {
        return bytes[offset] | (bytes[offset + 1] << 8);
    }

    function u32(bytes, offset)
    {
        return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0;
    }

    function i32(bytes, offset)
    {
        return u32(bytes, offset) | 0;
    }

    function le16(value)
    {
        return [value & 0xff, (value >>> 8) & 0xff];
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

    // SPEC-ucb.md section 4.2.
    function identity(bytes)
    {
        const constantCount = u32(bytes, 148);
        const mapBytes = Math.ceil(constantCount / 8);
        const section = {
            bytes,
            magic: u32(bytes, 0),
            version: u16(bytes, 4),
            coreKind: bytes[6],
            provenance: bytes[7],
            key: hex(bytes.subarray(8, 48)),
            holderDigest: hex(bytes.subarray(112, 144)),
            features: u16(bytes, 144),
            lexicallyScopedFeatures: bytes[146],
            hasCapturedVariables: bytes[147],
            constantCount,
            atomMap: 152,
            butterflyMap: 152 + mapBytes,
            end: 152 + 2 * mapBytes,
        };
        section.atom = index => bit(bytes, section.atomMap, index);
        section.butterfly = index => bit(bytes, section.butterflyMap, index);
        if (section.magic !== 0x49424355 || section.version !== 1 || align(section.end, 8) !== bytes.length)
            throw failure("ucb.identity does not have the layout of SPEC-ucb.md section 4.2", bytes);
        return section;
    }

    // SPEC-ucb.md section 5.2.
    function feedback(bytes)
    {
        const counts = {
            valueProfiles: u32(bytes, 8),
            arrayProfiles: u32(bytes, 12),
            binaryArithProfiles: u32(bytes, 16),
            unaryArithProfiles: u32(bytes, 20),
            exitSites: u32(bytes, 24),
            functionDecls: u32(bytes, 28),
            functionExprs: u32(bytes, 32),
            constants: u32(bytes, 52),
        };
        const predictions = 56;
        const arrays = predictions + 8 * counts.valueProfiles;
        const binary = arrays + 8 * counts.arrayProfiles;
        const unary = binary + 2 * counts.binaryArithProfiles;
        const exits = align(unary + 2 * counts.unaryArithProfiles, 4);
        const children = exits + 8 * counts.exitSites;
        const constants = children + counts.functionDecls + counts.functionExprs;
        const end = constants + Math.ceil(counts.constants / 8);
        const section = { bytes, counts, predictions, arrays, binary, unary, exits, children, constants, end };
        if (u32(bytes, 0) !== 0x46424355 || u16(bytes, 4) !== 1 || align(end, 8) !== bytes.length)
            throw failure("ucb.feedback does not have the layout of SPEC-ucb.md section 5.2", bytes);
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

    function varintBytes(value)
    {
        const bytes = [];
        while (value >= 0x80) {
            bytes.push((value & 0x7f) | 0x80);
            value = Math.floor(value / 128);
        }
        bytes.push(value);
        return bytes;
    }

    const zigzag = encoded => (encoded % 2 ? -(encoded + 1) / 2 : encoded / 2);

    // CachedJSValue::Kind and SourceCodeRepresentation, as runtime/CachedTypes.cpp and runtime/JSCJSValue.h number them.
    const constantKind = { Int32: 5, Double: 6, SymbolTable: 7, String: 8, ImmutableButterfly: 9, RegExp: 10, OrderedHashTableSentinel: 13 };
    const kindCount = 14;
    const linkTimeConstantRepresentation = 3;

    // A function's ucb.core (SPEC-ucb.codec.md section 3; CachedCodeBlock::packLayout in runtime/CachedTypes.cpp).
    function functionCore(bytes)
    {
        if (u32(bytes, 0) !== 0x43424355 || bytes[4] !== 3)
            throw failure("ucb.core is no function core", bytes);
        const rootOffset = u32(bytes, 8);
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
        core.slotsStart = core.poolStart + align(core.constantCount, 4);
        core.representationsStart = regionBegin + representations.at;
        core.kind = index => bytes[core.poolStart + index];
        core.slot = index => core.slotsStart + 4 * index;
        core.representation = index => (representations.count.value ? bytes[core.representationsStart + index] : 0);

        const inside = (start, length) => start >= 16 && start + length <= bytes.length;
        let consistent = regionBegin >= 16 && regionBegin <= rootOffset
            && inside(core.instructionsStart, instructions.count.value)
            && (!core.constantCount || inside(core.poolStart, align(core.constantCount, 4) + 4 * core.constantCount))
            && (!representations.count.value || inside(core.representationsStart, representations.count.value));
        for (let i = 0; consistent && i < core.constantCount; ++i)
            consistent = core.kind(i) < kindCount;
        // The expression info is reached through the record's CachedPtr: a 4-aligned offset from the slot itself.
        const expressionInfo = i32(bytes, rootOffset);
        consistent = consistent && !(expressionInfo & 3) && inside(rootOffset + expressionInfo, 4);
        if (!consistent)
            throw failure(`ucb.core does not have the layout this test reads: ${JSON.stringify({ rootOffset, flags, regionBegin, instructions: instructions.count.value, constants: core.constantCount, expressionInfo })}`, bytes);
        return core;
    }

    return {
        sha256, hex, ascii, sourceDigest, programKeyHex,
        u16, u32, i32, le16, le32, align, bit, find, varintBytes,
        identity, feedback, functionCore, constantKind, linkTimeConstantRepresentation,
    };
})();
