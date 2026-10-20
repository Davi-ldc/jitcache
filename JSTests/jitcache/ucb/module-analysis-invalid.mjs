// jitcache-host: bun
// jitcache-requires: twins
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-bench-report=analysis.bench; ConsumerProducer
// jitcache-expect-fault: 2 ucb.analysis
// jitcache-expect-no-install: 1
// jitcache-expect-no-install: 2

// A damaged saved analysis is invalid material, and the module then loads natively. Each sequence damages one rule of
// the strict check. The script imports module-analysis/ma-invalid.mjs dynamically, last; its top level runs once, so the
// Producer's delta at exit captures it without an image, with its analysis. The Consumer imports that analysis, and the
// last module-analysis event of its bench report, a hit, gives the module's key, which a Bun script has no other way to
// name. The ConsumerProducer rewrites the body's ucb.analysis at that key through bun:jsc's jitcacheRewriteSection,
// which only a producing VM may call, then imports the module: the strict check rejects the analysis, which is invalid
// material at ucb.analysis, and the module loads natively. The module's body has no image, so the Consumer and the
// ConsumerProducer install nothing. Every run equals the JITCache-off run.
import fs from "node:fs";
import * as jsc from "bun:jsc";
import ucbBun from "./resources/ucb-bun.js";

const t = ucbBun.context();

// Each damage as the bytes written at an offset of the 36-byte section ma-invalid.mjs describes.
const damages = [
    { what: "an offset width of 3", offset: 0, bytes: [3] },
    { what: "the TypeScript flag", offset: 18, bytes: [0x02] },
    { what: "a kind 1 record", offset: 30, bytes: [0x01] },
    { what: "a fetch kind 1", offset: 30, bytes: [0x04 | (1 << 3)] },
    { what: "an id past n + 1", offset: 32, bytes: [4] },
    // The first record becomes a single import whose import name is its local name, the ids its module name 0 and its
    // import name n + 1.
    { what: "n + 1 as an import name", offset: 30, bytes: [0x40, 0x04, 0x00, 0x03] },
    { what: "a duplicate export name", offset: 34, bytes: [0x00] },
    { what: "a record count one below the records written", offset: 26, bytes: [0x01] },
];
const damage = damages[t.sequence];

if (t.role === "ConsumerProducer" && t.twins) {
    const events = fs.readFileSync(t.file("analysis.bench"), "utf8").split("\n").filter(Boolean).map(line => JSON.parse(line))
        .filter(event => event.event === "module-analysis");
    const last = events.at(-1);
    t.check(last && last.outcome === "hit", () => `the Consumer's last module-analysis event is ${JSON.stringify(last)}, not a hit`);
    t.check(last.bytes === 36, () => `the module's analysis is ${last.bytes} bytes, not the 36 the damages assume`);
    jsc.jitcacheRewriteSection(last.key, "ucb.analysis", damage.offset, damage.bytes);
}

const invalid = await import("./module-analysis/ma-invalid.mjs");
console.log(`${damage.what}: ${Object.keys(invalid).join(",")} ${invalid.ka + invalid.kb}`);
t.finish();
