// jitcache-runs: Producer

// Harness sub-SPEC H5: one module binding that differs between the Producer run and the oracle's Off run changes the
// end-of-run description, which walks the environment of each module record the loader holds. The runner's self-test
// expects run 0 to fail on its heap description alone.
export let h5Binding = arguments[0] === "Producer" ? "the Producer's value" : "every other run's value";
