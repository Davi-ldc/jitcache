// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1
// jitcache-fixture: passes

// no-install.js with the Consumer declared to install nothing, which passes.
(function main() {
    print("nothing here runs often enough to compile");
})();
