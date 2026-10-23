// jitcache-runs: Producer --jitcache-test-twin-entry=coincidence:executable-pool
// jitcache-expect-exit: 0 abort
// jitcache-expect-twin: 0 coincidence executable-pool
// jitcache-pin: off its Off run aborts on purpose, and the pin comparison fails a run that ends by a signal
// jitcache-fixture: passes

// The expect-exit directive accepts a run that ends by SIGABRT, which writes no final status and no heap description.
// The shell writes the declared coincidence to the twin report right after start, and the run then aborts, so the
// report holds that line only if each line reaches the file as it is written. The script prints nothing,
// and its Off run aborts too.
(function main() {
    $vm.abort();
})();
