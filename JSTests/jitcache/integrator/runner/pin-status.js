// jitcache-runs: Producer

// Harness sub-SPEC H7: a script whose pin run ends with another status fails the sequence. Every twins-mode run takes
// --useDollarVM=true and ends cleanly, except this build's side of the pin comparison when the self-test gives it
// --pin-options=--useDollarVM=false: it throws, and the shell exits with 3. The comparison checks the status first.
// pin-status-declared.js is this script with jitcache-pin: off, which passes.
(function main() {
    if (typeof $vm === "undefined")
        throw new Error("this run has no $vm");
})();
