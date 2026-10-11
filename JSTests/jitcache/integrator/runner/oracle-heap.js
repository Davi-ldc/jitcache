// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Harness sub-SPEC H1: the Consumer leaves a value reachable from a global binding that its Off run does not, and prints
// what the Off run prints, so the oracle fails run 1 on its heap description alone. The failure also shows that the
// oracle's run wrote oracle0.heap: had it written the Consumer's run1.heap, the two files compared would be one.
load("./resources/warm-up.js", "caller relative");
var oracleHeapMarker = arguments[0] === "Consumer" ? "the Consumer's value" : "every other run's value";
(function main() {
    print(warmUp());
})();
