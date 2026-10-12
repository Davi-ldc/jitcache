// jitcache-host: bun
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer --jitcache-strict=0
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer

// SPEC-ucb.md section 13.3, supplied-digests.js, sections 3.5, 7.2.6 and S2: a `bun build --compile` executable whose
// modules carry the source digests the build recorded (Flags::HAS_JITCACHE_SOURCE_DIGESTS), run as a JITCache VM of its
// own with this run's role (resources/ucb-bun.js, runApp). The app, resources/supplied-digests-app.js with
// resources/supplied-digests-module.js embedded beside it, makes the checks; the sequence index picks the case:
// 0. Built with --bytecode, strict on: no source digest is computed while its modules and the internal modules it loads
//    are decoded (over the load of the embedded module and node:querystring, sourceDigests unchanged and
//    suppliedSourceDigests counting both), its decoded functions are seeded, a direct eval in a decoded function imports
//    with no verification although nothing has verified its module yet, and `new` on an ES5 constructor then imports its
//    construct body, which the ahead-of-time bytecode lacks (F2), after one verification of its module
//    (suppliedDigestVerifications).
// 1. The same with strict off in the Consumer: nothing is verified.
// 2. Built without --bytecode: its roots import, and each module is verified once, at the first import that rests on it.
// 3. Built without --bytecode, with the digest record removed from the executable as a Bun without the record would have
//    written it: its modules' digests are computed from their text, and they import as before, verifying nothing.
// 4. and 5. Built as CommonJS without --bytecode, so that the embedded module is CommonJS and require evaluates it through
//    JSCommonJSModule::evaluate; a build without --format=cjs makes it an ES module, which no module wrapper touches. The
//    app overrides the module wrapper (Module.wrapper; SPEC-ucb.md section 13.3, this script's row) before it loads
//    that module, in the Producer (4) or in the Consumer (5). The overridden text no longer has the recorded digest, which
//    JSCommonJSModule::evaluate clears, so the overriding run computes the module's digest from its text and keys its bodies
//    by it: neither run imports a body the other captured, and strict raises no invalid material.
// T7 holds in twins runs (section 13.2), and every run, the app's output included, equals the JITCache-off run.
//
// Bun calls delta at exit for every producing VM, the app's included.
const fs = require("node:fs");
const path = require("node:path");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const entries = ["supplied-digests-app.js", "supplied-digests-module.js"].map(name => path.join(__dirname, "resources", name));

// Clears Flags::HAS_JITCACHE_SOURCE_DIGESTS in the graph's Offsets, the 32 bytes before its trailer, whose last u32 is the
// flags (src/standalone_graph/StandaloneModuleGraph.rs). The graph is the last thing in the executable that holds the
// trailer; Bun's own constant comes earlier.
function removeDigestRecord(bytes)
{
    const trailer = Buffer.from("\n---- Bun! ----\n");
    const at = bytes.lastIndexOf(trailer);
    t.check(at >= 32, "the executable holds no module graph trailer");
    const flags = bytes.readUInt32LE(at - 4);
    const hasDigests = 1 << 11;
    t.check(flags & hasDigests, () => `the graph's flags ${flags.toString(16)} have no digest record`);
    bytes.writeUInt32LE((flags & ~hasDigests) >>> 0, at - 4);
}

let app;
if (t.sequence <= 1)
    app = t.buildApp("supplied-digests-bytecode-app", entries, { bytecode: true });
else if (t.sequence === 2)
    app = t.buildApp("supplied-digests-app", entries);
else if (t.sequence === 3)
    app = t.buildApp("supplied-digests-no-record-app", entries, { transform: removeDigestRecord });
else
    app = t.buildApp("supplied-digests-commonjs-app", entries, { format: "cjs" });
t.check(fs.existsSync(app), `no app at ${app}`);
t.runApp(app);
t.finish();
