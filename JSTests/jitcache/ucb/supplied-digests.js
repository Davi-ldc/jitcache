// jitcache-host: bun
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer

// Source digests a provider supplies, which strict verifies against the text once per provider before an import relies
// on one: a `bun build --compile` executable whose modules carry the source digests the build recorded
// (Flags::HAS_JITCACHE_SOURCE_DIGESTS), run as a JITCache VM of its own with this run's role and strict on
// (resources/ucb-bun.js, runApp). The app, resources/supplied-digests-app.js with
// resources/supplied-digests-module.js embedded beside it, makes the checks; the sequence index picks the case:
// 0. No source digest is computed over the load of the embedded module and of node:querystring, an internal module
//    (sourceDigests unchanged while suppliedSourceDigests counts both); each module's roots import after one
//    verification of its provider (suppliedDigestVerifications), `new` on an ES5 constructor imports its construct body
//    with no further verification, and a direct eval in an imported function imports with none. In the Consumer, the
//    lookup of an ES module's analysis when Bun creates its provider also hashes the text, which counts as one more
//    verification of the recorded digest.
// 1. With the digest record removed from the executable, as a Bun without the record would have written it: its
//    modules' digests are computed from their text, and they import as before. In the Consumer an ES module's digest is
//    computed by that lookup and handed to its provider, so its import verifies it as it verifies a recorded one.
// 2. and 3. Built as CommonJS, so that the embedded module is CommonJS and require evaluates it through
//    JSCommonJSModule::evaluate; a build without --format=cjs makes it an ES module, which no module wrapper touches. The
//    app overrides the module wrapper (Module.wrapper) before it loads that module, in the Producer (2) or in the
//    Consumer (3). The overridden text no longer has the recorded digest, which JSCommonJSModule::evaluate clears, so
//    the overriding run computes the module's digest from its text and keys its bodies by it: neither run imports a body
//    the other captured, and strict raises no invalid material.
// The engine's C++ tests check that imports with strict off verify nothing. In twins runs every supplied root digest
// equals the digest of the root's text, and every run, the app's output included, equals the JITCache-off run.
//
// The runs use the lane's default sequence, so the test's own VM calls delta at exit in the Producer; the app calls none
// (resources/ucb-bun.js).
const fs = require("node:fs");
const path = require("node:path");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const entries = ["supplied-digests-app.js", "supplied-digests-module.js", "warm-module.js"].map(name => path.join(__dirname, "resources", name));

// Clears Flags::HAS_JITCACHE_SOURCE_DIGESTS, bit 14, in the graph's Offsets, the 32 bytes before its trailer, whose last
// u32 is the flags (src/standalone_graph/StandaloneModuleGraph.rs). The graph is the last thing in the executable that
// holds the trailer; Bun's own constant comes earlier.
function removeDigestRecord(bytes)
{
    const trailer = Buffer.from("\n---- Bun! ----\n");
    const at = bytes.lastIndexOf(trailer);
    t.check(at >= 32, "the executable holds no module graph trailer");
    const flags = bytes.readUInt32LE(at - 4);
    const hasDigests = 1 << 14;
    t.check(flags & hasDigests, () => `the graph's flags ${flags.toString(16)} have no digest record`);
    bytes.writeUInt32LE((flags & ~hasDigests) >>> 0, at - 4);
}

let app;
if (t.sequence === 0)
    app = t.buildApp("supplied-digests-app", entries);
else if (t.sequence === 1)
    app = t.buildApp("supplied-digests-no-record-app", entries, { transform: removeDigestRecord });
else
    app = t.buildApp("supplied-digests-commonjs-app", entries, { format: "cjs" });
t.check(fs.existsSync(app), `no app at ${app}`);
t.runApp(app);
t.finish();
