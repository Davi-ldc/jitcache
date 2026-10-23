// jitcache-runs: Producer
// jitcache-expect-exit: 0 abort
// jitcache-fixture: runner --pin=$BUILD --extra-options=--useFTLJIT=false
// jitcache-fixture: fails 0 pin-crash
// jitcache-fixture: detail this build's run ended by SIG\w+.*; the pin's run ended by SIG\w+

// A script whose two pin runs both end by a signal fails the sequence as a crash, although their statuses and outputs
// are equal. The self-test runs it with --extra-options=--useFTLJIT=false, which every run takes, the oracle's and both
// pin runs included: each then aborts, and the directive accepts the Producer's abort. Without that option, as when the
// pin cases compare the whole directory, every run has the FTL on and exits with 0, so the two pin runs compare alike;
// the Producer then fails its exit check, which those cases do not read.
(function main() {
    if (typeof $vm !== "undefined" && !$vm.useFTLJIT())
        $vm.abort();
})();
