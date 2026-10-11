// jitcache-runs: Producer --jitcache-test-kill=after-rename@1000
// jitcache-expect-exit: 0 kill

// Harness sub-SPEC H8: a Producer that makes fewer commits than its kill point counts exits 0, which a directive that
// expects SIGKILL rejects.
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
