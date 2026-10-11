// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-delta-at-exit; Consumer

// Harness sub-SPEC H1: a ConsumerProducer followed by a Consumer prints a line its Off run does not, so the oracle fails
// run 1 on its output; the Producer and the Consumer print what their Off run prints and pass.
load("./resources/warm-up.js", "caller relative");
(function main(role) {
    print(warmUp());
    print(role === "ConsumerProducer" ? "the ConsumerProducer's line" : "every other run's line");
})(...arguments);
