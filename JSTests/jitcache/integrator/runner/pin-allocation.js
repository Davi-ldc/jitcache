// jitcache-runs: Producer

// Harness sub-SPEC H7: the pin comparison of this build with itself fails at the first block whose inline allocation
// became a jump to its slow path, when --pin-options=--forceGCSlowPaths=true reaches this build's side alone. make's
// object literal allocates inline in op_new_object's main path, and make reaches baseline code before main's loop does,
// in every run alike.
(function main() {
    function make(value) {
        return { value };
    }
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total += make(i).value;
    print(total);
})();
