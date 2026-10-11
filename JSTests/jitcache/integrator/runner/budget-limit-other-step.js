// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-max-memory=4096
// jitcache-expect-fault: 0 exec-alloc.dfg-plan

// Harness sub-SPEC H1: budget-limit.js with a directive that names another step, which leaves budget.limit undeclared
// and fails run 0.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
