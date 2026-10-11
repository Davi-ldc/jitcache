// jitcache-runs: Producer
// jitcache-pin: off it throws without $vm, which the self-test's --pin-options take away from this build's side

// Harness sub-SPEC H7: pin-status.js with jitcache-pin: off, which the comparison skips, so the script passes.
(function main() {
    if (typeof $vm === "undefined")
        throw new Error("this run has no $vm");
})();
