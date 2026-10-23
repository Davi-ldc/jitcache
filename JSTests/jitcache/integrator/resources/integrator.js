// What the integrator's JS tests share: assertions, readers of a body's sections and of a body's event counts, and the
// scratch files the runs of one sequence hand each other. This file binds the assertions and the byte reader of
// ../../resources/jitcache.js as globals beside its own. The runner gives every run its role, scratch path, artifact path
// and sequence index, and each script passes them to the function that holds its body, so this file's top level declares
// functions and binds the shared ones only, and nothing it leaves reachable depends on the role.
//
// The jitcache* shell functions exist only in twins builds, which every script of this directory requires. A script's
// Off path calls them only where hasTwins() holds. The pin has none of them, and the pin comparison runs this build
// without them too, so both of its sides skip those branches.
//
// A test's own code must not compile where a script counts compiles or commits. Every function here and in the shared
// file has a recorded key, so its baseline compilation is also a commit in a producing run, which a script that counts
// its writer's commits, as producer-kill.js does, would take for one of its own. The LLInt compiles a function at its
// 34th call, since a call earns 15 of the 500 points it needs, or once a loop in it has run about 500 times, and with
// --useLLInt=false at its first call. So a helper builds its message, and calls label, only once its check has failed,
// and a script calls hex only where an extra compilation or commit changes nothing it checks.

var jitcache = load("../../resources/jitcache.js", "caller relative");
var { fail, check, checkSame, byteReader } = jitcache;

const INT32_MAX = 0x7fffffff;

// Assertions. Each throws, which fails the run on its exit code.

// Fails unless actual and expected, each an ArrayBuffer or a Uint8Array, hold the same bytes, and names the first
// difference rather than printing every byte. A null actual is a section the body lacks.
function checkSameBytes(actual, expected, what) {
    if (actual === null)
        fail(`${what} is missing`);
    const actualBytes = new Uint8Array(actual);
    const expectedBytes = new Uint8Array(expected);
    if (actualBytes.length !== expectedBytes.length)
        fail(`${what} holds ${actualBytes.length} bytes, expected ${expectedBytes.length}`);
    for (let i = 0; i < actualBytes.length; ++i) {
        if (actualBytes[i] !== expectedBytes[i])
            fail(`${what} differs at byte ${i}: ${actualBytes[i]}, expected ${expectedBytes[i]}`);
    }
}

function hasTwins() {
    return typeof jitcacheBodyEvents === "function";
}

// Bodies and sections.

function label(fn, kind) {
    return `${fn.name || "an anonymous function"} (${kind})`;
}

// The key the parent-key registry recorded for the UCB of fn's CodeBlock of kind; the function must have run.
function bodyKey(fn, kind = "call") {
    const key = jitcacheBodyKey(fn, kind);
    if (!key)
        fail(`${label(fn, kind)} has no recorded key`);
    return key;
}

// The section of that key's current body file, by its name such as "cb.summary", as an ArrayBuffer, or null when the
// key holds no body.
function sectionOf(fn, kind, name) {
    return jitcache.readSection(bodyKey(fn, kind), name);
}

function hasBody(fn, kind = "call") {
    return sectionOf(fn, kind, "ucb.identity") !== null;
}

// The bytes of an ArrayBuffer or a Uint8Array as lowercase hex, or null for null. It calls the iterator builtins and
// padStart for every byte, so they compile on all but the shortest sections, and hex itself does once its loop has run
// about 500 times (above).
function hex(buffer) {
    if (buffer === null)
        return null;
    let text = "";
    for (const byte of new Uint8Array(buffer))
        text += byte.toString(16).padStart(2, "0");
    return text;
}

// Event counts.

function events(fn, kind = "call") {
    const counts = jitcache.bodyEvents(fn, kind);
    if (!counts)
        fail(`${label(fn, kind)} has no CodeBlock whose events could be read`);
    return counts;
}

// A body whose first CodeBlock installed an import ran baseline code from its first call: the LLInt began no instruction
// of it and nothing compiled it.
function expectInstalled(fn, kind = "call") {
    const counts = events(fn, kind);
    if (counts.llintInstructions !== 0 || counts.baselineCompiles !== 0)
        fail(`${label(fn, kind)}, which should have installed an import, began ${counts.llintInstructions} LLInt instructions and compiled ${counts.baselineCompiles} times`);
}

// A body that ran in the LLInt until its own baseline compilation.
function expectCompiledNatively(fn, kind = "call") {
    const counts = events(fn, kind);
    if (!(counts.llintInstructions > 0) || counts.baselineCompiles !== 1)
        fail(`${label(fn, kind)}, which should have compiled natively once, began ${counts.llintInstructions} LLInt instructions and compiled ${counts.baselineCompiles} times`);
}

// The change of one jitcacheProgress() field between two readings.
function grew(before, after, field) {
    return after[field] - before[field];
}

// Scratch files, which every run of a sequence shares.

function saveText(scratch, name, text) {
    if (writeFile(`${scratch}/${name}`, text) < 0)
        fail(`cannot write ${scratch}/${name}`);
}

function loadText(scratch, name) {
    return readFile(`${scratch}/${name}`);
}
