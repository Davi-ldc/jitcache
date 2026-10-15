// jitcache-host: bun

// A `bun build --compile --bytecode` executable, whose UCBs Bun decodes from the persistent payload it embeds, so their
// instruction streams are borrowed from the executable's section. The app, resources/borrowed-bytecode-app.js, runs as
// a JITCache VM of its own with this run's role over an artifact of its own (resources/ucb-bun.js, runApp): in the
// Producer its functions run until they are
// captured, which encodes their borrowed streams, and in the Consumer each decoded function is seeded at its first call
// (statistics, checked inside the app). The app's output, which this run prints, equals the JITCache-off run.
//
// The runs use the lane's default sequence, so the test's own VM calls delta at exit in the Producer; the app calls
// none (resources/ucb-bun.js).
const path = require("node:path");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const app = t.buildApp("borrowed-bytecode-app", [path.join(__dirname, "resources", "borrowed-bytecode-app.js")], { bytecode: true });
t.runApp(app);
t.finish();
