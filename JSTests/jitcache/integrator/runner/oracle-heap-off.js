// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-heap: off the script leaves its role in a global binding on purpose, to show that the oracle then compares its output alone

// Harness sub-SPEC H1: oracle-heap.js with jitcache-heap off, which passes.
load("./resources/warm-up.js", "caller relative");
var oracleHeapMarker = arguments[0] === "Consumer" ? "the Consumer's value" : "every other run's value";
(function main() {
    print(warmUp());
})();
