// jitcache-requires: twins
// jitcache-expect-fault: 1 ucb.identity
// jitcache-expect-fault: 1 ucb.decode
// jitcache-expect-no-install: 1
// jitcache-runs: 3x Producer; Consumer

// A body whose ucb.identity holds another key, or whose core fails the decode's own checks, is invalid material at the
// step that finds it, cache activity turns off, and the run goes on as the JITCache-off run does.
// resources/invalid-material-cases.js runs each case: the Producer captures every body and rewrites one section of one,
// and the Consumer, run 1, imports it and checks the step. Run 1 may therefore report each of the two steps these cases
// reach, and installs nothing it has to.
//
// Each sequence above is one case of the table below, which the sequence index picks:
// - ucb.identity: a stored key other than the body's, which the import compares with the key the body is filed under;
// - ucb.decode: a symbol record naming a private name this VM lacks (a private name record turned into a well-known
//   symbol that does not exist) and a truncated payload (a root record offset at its end).
// jitcacheRewriteSection never changes a section's size, so a truncation is written as a root record offset that leaves
// no room for the record, which the decode reads as the payload ending there.
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");
load("./resources/invalid-material-cases.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    invalidMaterialCases.run(ucbTest(role, scratch, artifact, sequence), 3, c => [
        c.identityCase("a stored key other than the body's", identity => [8, [identity.bytes[8] ^ 1]]),

        c.coreCase("a private name this VM lacks", "ucb.decode", "imPrivate", core => {
            // The record of @privateBrand: 12 Latin-1 characters, a private and unregistered symbol, no ordinal. Marking it
            // well-known keeps its characters and hash, and lookUpWellKnownSymbol finds no such symbol.
            const at = c.S.find(core.bytes, [0x0c, 0, 0, 0x98, null, null, null, null, 0xff, 0xff, 0xff, 0xff, ...c.S.ascii("privateBrand")]);
            c.t.check(at >= 0, "imPrivate's core holds no @privateBrand record");
            return [at, [0x0c, 0, 0, 0x38]];
        }),
        c.coreCase("a truncated payload", "ucb.decode", "imPlain", core => [8, c.S.le32(c.S.align(core.bytes.length, 4))]),
    ]);
})(...arguments);
