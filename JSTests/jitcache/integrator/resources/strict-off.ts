// default-mode.js's checker for the runs of its default sequence: the run started, and --jitcache-strict=0, which comes
// after the runner's --jitcache-strict=1, left strict off, the default the bench measures.
import { checkFinalStatus } from "./checker.ts";

checkFinalStatus({ state: "started", strict: false });
