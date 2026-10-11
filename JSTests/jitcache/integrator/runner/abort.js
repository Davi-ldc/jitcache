// jitcache-runs: Producer --jitcache-test-twin-entry=coincidence:executable-pool
// jitcache-expect-exit: 0 abort
// jitcache-expect-twin: 0 coincidence executable-pool

// Harness sub-SPEC H1: jitcache-expect-exit accepts a run that ends by SIGABRT, which writes no final status and no heap
// description. H4: the shell writes the declared coincidence to the twin report right after start, and the run then
// aborts, so the report holds that line only if each line reaches the file as it is written. The script prints nothing,
// and its Off run aborts too.
(function main() {
    $vm.abort();
})();
