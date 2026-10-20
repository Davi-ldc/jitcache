// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-delta-at-exit; Consumer

// A recaptured module body keeps its analysis. module-analysis/ma-recapture.mjs loops a few rounds in the Producer, so
// its top level runs once without compiling and the Producer's delta at exit captures it without an image, with its
// analysis. The ConsumerProducer imports that analysis and builds the module's record from it, so the record keeps no
// analysis source; its top level loops long enough to compile, and the finalize capture of that compilation beats the
// saved body without an image, copying the saved body's analysis (analysisCopied). The Consumer then imports the
// analysis from the recaptured body and installs its image. A function every run calls often enough to compile gives
// the ConsumerProducer a body to install too. Every run equals the JITCache-off run.
import ucbBun from "./resources/ucb-bun.js";
import { maMissHot } from "./module-analysis/ma-miss-hot.mjs";

const t = ucbBun.context();

let total = 0;
for (let i = 0; i < 1000; ++i)
    total = (total + maMissHot(i)) | 0;
console.log(`hot ${total}`);

const before = t.statistics();
const recaptured = await import("./module-analysis/ma-recapture.mjs");
const after = t.statistics();
const expectations = { };
if (t.imports)
    expectations.analysisImports = 1;
if (t.role === "ConsumerProducer")
    expectations.analysisCopied = [">=", 1];
t.expect("the module's import", t.delta(before, after), expectations);
console.log(`recaptured ${recaptured.maRecaptured}`);
t.finish();
