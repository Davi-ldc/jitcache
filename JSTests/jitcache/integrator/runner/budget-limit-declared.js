// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-max-memory=4096
// jitcache-expect-fault: 0 budget.limit

// Harness sub-SPEC H1: budget-limit.js with its fault declared, which passes.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
