// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1

// Harness sub-SPEC H1: no-install.js with the Consumer declared to install nothing, which passes.
(function main() {
    print("nothing here runs often enough to compile");
})();
