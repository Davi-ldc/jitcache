// jitcache-runs: Producer

// Harness sub-SPEC H7: a script whose pin run prints something else fails the sequence. Every twins-mode run takes
// --useDollarVM=true and prints "object", except this build's side of the pin comparison when the self-test gives it
// --pin-options=--useDollarVM=false. pin-output-declared.js is this script with jitcache-pin: off, which passes.
(function main() {
    print(typeof $vm);
})();
