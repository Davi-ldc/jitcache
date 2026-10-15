// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Module code in imported images, each import twin-checked. Functions of this module read imported bindings through
// resolve_scope and get_from_scope of type ModuleVar, which a baked fact records, read and write the module's own
// variables as closure variables over its environment, and write an import, which put_to_scope's ModuleVar dispatch
// answers with the read-only TypeError through its slow-path call, which the consumer checks the images carry. The
// imported module's function updates its live binding. Every binding left in the two module environments, which the
// heap description walks, is the same in every run.
import { counter, increment, label } from "./resources/module-dependency.mjs";

let moduleCount = 0;
export let exportedCount = 0;

function readImports(n) {
    return counter + label.length + n;
}
function writeImport(n) {
    try {
        counter = n;
        return "written";
    } catch (error) {
        return error.name;
    }
}
function bump(n) {
    moduleCount += n;
    exportedCount += 1;
    return moduleCount + exportedCount;
}

let total = 0;
const outcomes = new Set();
for (let i = 0; i < 60; ++i) {
    total += readImports(i) + increment(i % 3) + bump(i);
    outcomes.add(writeImport(i));
}
print(total);
print([...outcomes].join(","));
print(counter, moduleCount, exportedCount);

// The case happened (HARNESS.md, Tests): the producer compiled the four functions the loop calls, so each ran its import
// here. A module reads its role from the shell's global arguments, and the check runs inside a function of the role, so
// it leaves no binding in the module's environment.
(function checkImports(role) {
    if (role !== "Consumer")
        return;
    const sections = load("./resources/image-sections.js", "caller relative");
    const bodies = { readImports, writeImport, bump, increment };
    sections.expectImports(role, bodies);
    sections.expectTargets(role, { call: bodies }, [
        { site: "D6, A1", kind: "SlowPathThunk", form: "Call" },
    ]);
})(globalThis.arguments[0]);
