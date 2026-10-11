// jitcache-runs: Producer --jitcache-test-kill=after-rename@1
// jitcache-expect-exit: 0 abort

// Harness sub-SPEC H8: a run that ends by SIGKILL fails a directive that expects SIGABRT.
(function main() {
    function h8First(x) {
        return x + 1;
    }
    function h8Second(x) {
        return x * 2;
    }
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total += h8First(i) + h8Second(i);
})();
