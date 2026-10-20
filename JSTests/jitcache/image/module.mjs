// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Module code in imported images, each import twin-checked. Functions of this module read imported bindings through a
// resolve_scope of type ModuleVar, which walks its scope depth and then loads the import's slot in this module's
// environment, and a get_from_scope of type LazyClosureVar; they read the module's own function declarations through a
// get_from_scope of type LazyClosureVar as well, and baked facts record all three. readImports and readDeclared read
// them at depth 0, and the innermost function deepReader returns at a depth above eight, where resolve_scope loops.
// Each import's slot and each declaration's slot stays empty until the first read of it, and each is first read by one
// of these functions, which in a consumer runs imported code from its first call, so both slow cases, which call the
// GlobalVar scope thunks, run there. The functions also read and write the module's own variables as closure
// variables over its environment, and write an import, which put_to_scope's ModuleVar dispatch answers with the
// read-only TypeError through its slow-path call. The consumer checks that the images carry these calls. The imported
// module's function updates its live binding. Every binding left in the two module environments, which the heap
// description walks, is the same in every run.
import { counter, deepLabel, increment, label } from "./resources/module-dependency.mjs";

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
function declared(n) {
    return n * 2;
}
function deepDeclared(n) {
    return n + 3;
}
function readDeclared(n) {
    return declared(n) + 1;
}
function deepReader() {
    let d1 = 1;
    return (() => {
        let d2 = d1 + 1;
        return (() => {
            let d3 = d2 + 1;
            return (() => {
                let d4 = d3 + 1;
                return (() => {
                    let d5 = d4 + 1;
                    return (() => {
                        let d6 = d5 + 1;
                        return (() => {
                            let d7 = d6 + 1;
                            return (() => {
                                let d8 = d7 + 1;
                                return (() => {
                                    let d9 = d8 + 1;
                                    return function innermost(n) {
                                        return deepLabel.length + deepDeclared(n) + d9;
                                    };
                                })();
                            })();
                        })();
                    })();
                })();
            })();
        })();
    })();
}

const innermost = deepReader();
let total = 0;
const outcomes = new Set();
for (let i = 0; i < 60; ++i) {
    total += readImports(i) + increment(i % 3) + bump(i) + readDeclared(i) + innermost(i);
    outcomes.add(writeImport(i));
}
print(total);
print([...outcomes].join(","));
print(counter, moduleCount, exportedCount);

// The case happened: the producer compiled the functions the loop calls, so each ran its import here. A module reads its
// role from the shell's global arguments, and the check runs inside a function of the role, so it leaves no binding in
// the module's environment.
(function checkImports(role) {
    if (role !== "Consumer")
        return;
    const sections = load("./resources/image-sections.js", "caller relative");
    const bodies = { readImports, writeImport, bump, increment, readDeclared, declared, innermost };
    sections.expectImports(role, bodies);
    sections.expectTargets(role, { call: bodies }, [
        { site: "a ModuleVar put_to_scope's slow-path call", kind: "SlowPathThunk", form: "Call" },
    ]);
    // The slow cases of a ModuleVar resolve_scope and of a LazyClosureVar get_from_scope, which call the GlobalVar
    // scope thunks, in the image of each function that reads an import or a declaration, at depth 0 and above eight.
    // readDeclared reads no import, so its image holds only the second.
    const moduleVarSlowCase = {
        site: "the ModuleVar resolve_scope's slow case", kind: "BaselineThunk", a: "ResolveScopeGlobalVar", form: "Call",
    };
    const lazyClosureVarSlowCase = {
        site: "the LazyClosureVar get_from_scope's slow case", kind: "BaselineThunk", a: "GetFromScopeGlobalVar", form: "Call",
    };
    sections.expectTargets(role, { call: { readImports } }, [moduleVarSlowCase, lazyClosureVarSlowCase]);
    sections.expectTargets(role, { call: { readDeclared } }, [lazyClosureVarSlowCase]);
    sections.expectTargets(role, { call: { innermost } }, [moduleVarSlowCase, lazyClosureVarSlowCase]);
})(globalThis.arguments[0]);
