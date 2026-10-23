// jitcache-runs: Off
// jitcache-runs: 2x Off
// jitcache-cadence: milestone 1
// jitcache-cadence: release 2
// jitcache-fixture: runner --milestone
// jitcache-fixture: fails 1:0 exit
// jitcache-fixture: lists sequence 2 runs only on release builds

// A cadence keeps some sequences of a script to milestones or to one kind of build. Sequence 1 runs only under
// --milestone, which the self-test gives the runner here, and sequence 2 only on release builds, while the self-test
// runs a twins build, whose build type is Debug. Both throw if they run, so the runner fails sequence 1 alone and lists
// sequence 2, and sequence 0, which every build runs, passes. cadence-script.js leaves a whole script to milestones.
(function main(role, scratch, artifact, sequence) {
    if (sequence !== "0")
        throw new Error(`sequence ${sequence} ran, and its cadence lets it run only under --milestone or on release builds`);
})(...arguments);
