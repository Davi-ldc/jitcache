// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Nothing runs often enough to compile, so the Producer commits no body and the Consumer installs
// none, which fails run 1.
(function main() {
    print("nothing here runs often enough to compile");
})();
