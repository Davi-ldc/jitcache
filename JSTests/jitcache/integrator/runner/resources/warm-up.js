// The runner's fixtures load this file to give every run a body that compiles to baseline code: hot's 1000 calls, and
// warmUp's loop, tier up in every run, so a Producer commits them and a Consumer installs them. The result, and what
// the file leaves reachable, do not depend on the run's role.
function warmUp() {
    function hot(x) {
        return x * 3 + 1;
    }
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total += hot(i);
    return total;
}
