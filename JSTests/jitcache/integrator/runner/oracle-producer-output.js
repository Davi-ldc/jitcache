// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Harness sub-SPEC H1: the Producer prints a line its Off run does not. The oracle compares producers too, so it fails
// run 0 on its output, and nothing else fails.
load("./resources/warm-up.js", "caller relative");
(function main(role) {
    print(warmUp());
    print(role === "Producer" ? "the Producer's line" : "every other run's line");
})(...arguments);
