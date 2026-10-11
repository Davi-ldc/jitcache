// jitcache-runs: Producer
// jitcache-pin: off it prints whether $vm exists, which the self-test's --pin-options take away from this build's side

// Harness sub-SPEC H7: pin-output.js with jitcache-pin: off, which the comparison skips, so the script passes.
(function main() {
    print(typeof $vm);
})();
