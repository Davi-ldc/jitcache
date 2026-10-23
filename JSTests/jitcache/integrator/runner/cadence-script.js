// jitcache-runs: Off
// jitcache-cadence: milestone
// jitcache-fixture: skipped runs only with --milestone

// A cadence that names no sequence keeps the whole script to milestones, so without --milestone the runner skips the
// script and says why. The script throws if it runs. cadence.js keeps single sequences to a cadence.
(function main() {
    throw new Error("the script ran without --milestone, which its cadence requires");
})();
