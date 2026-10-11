// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Harness sub-SPEC H1: the Consumer prints a line its Off run does not, so the oracle fails run 1 on its output, and
// nothing else fails. The runner's self-test (self-test.ts) expects exactly that failure.
load("./resources/warm-up.js", "caller relative");
(function main(role) {
    print(warmUp());
    print(role === "Consumer" ? "the Consumer's line" : "every other run's line");
})(...arguments);
