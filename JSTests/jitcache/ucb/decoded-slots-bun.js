// jitcache-host: bun

// SPEC-ucb.md section 13.3, decoded-slots.js, the variant marked Bun: the slots of functions Bun decodes lazily from a
// `bun build --compile --bytecode` executable. The app, resources/decoded-slots-bun-app.js, runs as a JITCache VM of its
// own with this run's role (resources/ucb-bun.js, runApp) and checks, in the Consumer, that the requested slot of each
// lazily decoded function is seeded at its first call and its child at its own; that `new` first on an ordinary
// function, whose call body alone the executable holds, records the decoded call slot without settling the request and
// imports the empty construct slot; that the decoded call slot attaches at its own first request when its only string
// constant is an inline string, and misses AtomMap without a stamp when a longer constant decoded plain, attaching at a
// later request once its function's run used that constant's value as a property key, which made it an atom. The app
// keeps that constant's characters out of its other strings, since such a build keeps every string of four or more
// characters in one shared table, where an identifier with those characters would make the constant an atom at its
// decode. T4 holds at every seeding (section 13.2). The app's output, which this run prints, equals the JITCache-off
// run.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4); Bun calls delta at exit for every
// producing VM, the app's included.
const path = require("node:path");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const app = t.buildApp("decoded-slots-bun-app", [path.join(__dirname, "resources", "decoded-slots-bun-app.js")], { bytecode: true });
t.runApp(app);
t.finish();
