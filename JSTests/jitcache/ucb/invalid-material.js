// jitcache-requires: twins
// jitcache-expect-fault: 1 ucb.identity
// jitcache-expect-fault: 1 ucb.decode
// jitcache-expect-fault: 1 ucb.closure
// jitcache-expect-no-install: 1
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer

// SPEC-ucb.md section 13.3, invalid-material.js, sections 4.2, 4.4 and 10, and SPEC-ucb.codec.md sections 3 and E15: with
// strict on, a body whose lane sections are malformed is invalid material at the step that finds it, cache activity turns
// off, and the run goes on as the JITCache-off run does. resources/invalid-material-cases.js runs each case: the Producer
// captures every body and rewrites one section of one, and the Consumer, run 1, imports it and checks the step. Run 1 may
// therefore report each of the three steps these cases reach, and installs nothing it has to.
//
// Each sequence above is one case of the table below, which the sequence index picks. The runner reads directives only in
// a script's first 50 lines, so the rules of ucb.feedback have a script of their own, invalid-material-feedback.js. Here:
// - ucb.identity, every rule of section 4.2 that a rewrite of the same size can break: the magic, the layout version, a
//   core kind past the last and one other than the key's, a provenance above 1, each field of the key that
//   BodyKey::fromBytes checks (its version, identity kind, specialization, code-generation mode and reserved bytes), a
//   stored key other than the body's, which step 4a compares in both modes, a holder digest and each function field on a
//   program body, each function field out of its range, a constant count of 1 << 28 or more, a bit at N in either map, a
//   constant marked in both maps and a nonzero padding byte;
// - ucb.closure: profile counts that no longer match the UCB (the declaration and expression counts swapped), a
//   link-time constant out of range, a RegExp constant whose flags make its pattern invalid, an exit site past the
//   instructions, an atom map that marks an Int32 constant, a butterfly map that marks it and a singleton bit on it;
// - ucb.decode: a symbol record naming a private name this VM lacks (a private name record turned into a well-known
//   symbol that does not exist), a truncated payload (a root record offset at its end), a nested offset outside the
//   payload, a string length past its record, an unknown constant kind, an unknown opcode and an instruction that runs
//   past the stream (the stream's count shortened by one).
// No same-size rewrite can give a section the wrong size; SPEC-ucb.md U5 checks that rule. jitcacheRewriteSection never
// changes a section's size, so a truncation is written as a root record offset that leaves no room for the record, which
// the decode reads as the payload ending there (SPEC-ucb.md section 13.3, this script's row).
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");
load("./resources/invalid-material-cases.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    invalidMaterialCases.run(ucbTest(role, scratch, artifact, sequence), 37, c => [
        c.identityCase("the identity magic", identity => [0, [identity.bytes[0] ^ 1]]),
        c.identityCase("the identity layout version", () => [4, [2, 0]]),
        c.identityCase("a core kind past the last", () => [6, [4]]),
        c.identityCase("a core kind other than the key's", () => [6, [0]]),
        c.identityCase("a provenance above 1", () => [7, [2]]),
        c.identityCase("a key version other than 1", () => [8, [2]]),
        c.identityCase("a key identity kind of 0", () => [9, [0]]),
        c.identityCase("a key specialization of 2", () => [10, [2]]),
        c.identityCase("a key code-generation mode bit CodeGenerationMode does not define", () => [11, [0x80]]),
        c.identityCase("a nonzero reserved byte in the key", () => [12, [1]]),
        c.identityCase("a stored key other than the body's", identity => [16, [identity.bytes[16] ^ 1]]),
        c.identityCase("a holder digest on a program body", () => [112, [1]], "program"),
        c.identityCase("function features on a program body", () => [144, [1, 0]], "program"),
        c.identityCase("lexically scoped features on a program body", () => [146, [1]], "program"),
        c.identityCase("a captured-variables byte on a program body", () => [147, [1]], "program"),
        c.identityCase("function features past bitWidthOfCodeFeatures", () => [144, [0xff, 0xff]]),
        c.identityCase("lexically scoped features past AllLexicallyScopedFeatures", () => [146, [0xff]]),
        c.identityCase("a captured-variables byte of 2", () => [147, [2]]),
        c.identityCase("a constant count of 1 << 28 more", identity => [151, [identity.bytes[151] | 0x10]]),
        c.identityCase("an atom map bit at N", identity => c.setBit(identity.bytes, identity.atomMap, identity.constantCount), c.unevenConstants),
        c.identityCase("a butterfly map bit at N", identity => c.setBit(identity.bytes, identity.butterflyMap, identity.constantCount), c.unevenConstants),
        c.identityCase("a string constant marked in both maps", (identity, body) => {
            const { index } = c.stringRecord(body.core(), 37);
            c.t.check(identity.atom(index), "the long string constant is not marked as an atom");
            return c.setBit(identity.bytes, identity.butterflyMap, index);
        }),
        c.identityCase("a nonzero padding byte", identity => [identity.end, [1]], c.paddedIdentity),

        c.sectionCase("swapped declaration and expression counts", "ucb.closure", "ucb.feedback", "imPlain", feedback => {
            const { functionDecls, functionExprs } = feedback.counts;
            c.t.check(functionDecls !== functionExprs, "imPlain's declaration and expression counts are equal");
            return [28, [...c.S.le32(functionExprs), ...c.S.le32(functionDecls)]];
        }),
        c.coreCase("a link-time constant out of range", "ucb.closure", "imLinkTime", core => {
            for (let i = 0; i < core.constantCount; ++i) {
                if (core.representation(i) === c.S.linkTimeConstantRepresentation && core.kind(i) === c.S.constantKind.Int32)
                    return [core.slot(i), c.S.le32(0x7fffff00)];
            }
            throw new Error("imLinkTime has no link-time constant");
        }),
        c.coreCase("a RegExp constant whose flags make its pattern invalid", "ucb.closure", "imRegExp", core => {
            for (let i = 0; i < core.constantCount; ++i) {
                if (core.kind(i) !== c.S.constantKind.RegExp)
                    continue;
                // CachedRegExp: pattern, atom, u16 subpattern count, then the u16 flags. /\-/ is valid until the u flag.
                const flags = core.slot(i) + c.S.i32(core.bytes, core.slot(i)) + 10;
                return [flags, c.S.le16(c.S.u16(core.bytes, flags) | 0x20)];
            }
            throw new Error("imRegExp has no RegExp constant");
        }),
        c.sectionCase("an exit site past the instructions", "ucb.closure", "ucb.feedback", "imExit", feedback => {
            c.t.check(feedback.counts.exitSites, "imExit has no exit site");
            return [feedback.exits, c.S.le32(0x7ffffff0)];
        }),
        c.sectionCase("an atom map that marks an Int32 constant", "ucb.closure", "ucb.identity", "imPlain",
            (identity, body) => c.setBit(identity.bytes, identity.atomMap, c.int32Constant(body.core(), 1000))),
        c.sectionCase("a butterfly map that marks an Int32 constant", "ucb.closure", "ucb.identity", "imPlain",
            (identity, body) => c.setBit(identity.bytes, identity.butterflyMap, c.int32Constant(body.core(), 1000))),
        c.sectionCase("a singleton bit on an Int32 constant", "ucb.closure", "ucb.feedback", "imPlain",
            (feedback, body) => c.setBit(feedback.bytes, feedback.constants, c.int32Constant(body.core(), 1000))),

        c.coreCase("a private name this VM lacks", "ucb.decode", "imPrivate", core => {
            // The record of @privateBrand: 12 Latin-1 characters, a private and unregistered symbol, no ordinal. Marking it
            // well-known keeps its characters and hash, and lookUpWellKnownSymbol finds no such symbol (codec E5).
            const at = c.S.find(core.bytes, [0x0c, 0, 0, 0x98, null, null, null, null, 0xff, 0xff, 0xff, 0xff, ...c.S.ascii("privateBrand")]);
            c.t.check(at >= 0, "imPrivate's core holds no @privateBrand record");
            return [at, [0x0c, 0, 0, 0x38]];
        }),
        c.coreCase("a truncated payload", "ucb.decode", "imPlain", core => [8, c.S.le32(c.S.align(core.bytes.length, 4))]),
        c.coreCase("a nested offset outside the payload", "ucb.decode", "imPlain", core => [core.rootOffset, c.S.le32(0x3ffffff0)]),
        c.coreCase("a string length past its record", "ucb.decode", "imPlain", core => {
            const { record } = c.stringRecord(core, 37);
            return [record, c.S.le32(((c.S.u32(core.bytes, record) & 0xf8000000) | 0x7ffffff) >>> 0)];
        }),
        c.coreCase("an unknown constant kind", "ucb.decode", "imPlain", core => [core.poolStart + c.int32Constant(core, 1000), [200]]),
        c.coreCase("an unknown opcode", "ucb.decode", "imPlain", core => [core.instructionsStart, [0xff]]),
        c.coreCase("an instruction that runs past the stream", "ucb.decode", "imPlain", core => {
            const count = core.instructions.count;
            const shorter = c.S.varintBytes(count.value - 1);
            c.t.check(shorter.length === count.length, "the instruction count's varint changes length");
            return [count.offset, shorter];
        }),
    ]);
})(...arguments);
