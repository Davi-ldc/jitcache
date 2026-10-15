// What the jsc-hosted scripts of JSTests/jitcache/ share: the runner's arguments, assertions, wrappers of the shell
// functions a twins build registers, each answering null where a run may not use its function, and a little-endian
// reader of a section's bytes. A file takes them with load("<path to this directory>/jitcache.js", "caller relative"),
// which returns the object at the end of this file and defines no global. A lane's section readers load only in the runs
// that read sections, and the oracle compares the heap a run leaves with an Off run's, so a global defined there would
// differ between the two. Every function reads the role when it is called, so loading this file leaves nothing reachable
// that depends on the role.
(function () {
    "use strict";

    // The runner's arguments. The shell's global arguments hold the role, the scratch directory, the artifact path and
    // the sequence index.

    function role() {
        return globalThis.arguments[0];
    }

    function sequence() {
        return Number(globalThis.arguments[3]);
    }

    // A file in the scratch directory, which every run of a sequence shares.
    function scratchPath(name) {
        return `${globalThis.arguments[1]}/${name}`;
    }

    // Assertions. Each throws, which fails the run on its exit code.

    function fail(message) {
        throw new Error(message);
    }

    function check(condition, message) {
        if (!condition)
            fail(message);
    }

    function checkSame(actual, expected, what) {
        if (actual !== expected)
            fail(`${what} is ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`);
    }

    // The shell functions. Plain builds register none of them, and a twins build registers none in the pin comparison's
    // runs, so each wrapper first tests that its function exists. An Off run, which the oracle's runs are, asserts nothing
    // about what JITCache did, so the status, delta and section wrappers answer null there as well. Body events count
    // what the engine did, which a run may check in every role, so their wrapper answers null only where the function is
    // missing.

    function configuresJITCache() {
        return role() !== "Off";
    }

    // The full step name of the VM's first fault, or null.
    function status() {
        return typeof jitcacheStatus === "function" && configuresJITCache() ? jitcacheStatus() : null;
    }

    // What jitcacheDelta() returns, and throws what it throws.
    function delta() {
        return typeof jitcacheDelta === "function" && configuresJITCache() ? jitcacheDelta() : null;
    }

    // The event counts of the UCB of fn's CodeBlock of kind, or null when fn has no CodeBlock of that kind.
    function bodyEvents(fn, kind = "call") {
        return typeof jitcacheBodyEvents === "function" ? jitcacheBodyEvents(fn, kind) : null;
    }

    // The section of that name in the body the artifact holds at key, as an ArrayBuffer, or null when there is none.
    function readSection(key, name) {
        return typeof jitcacheReadSection === "function" && configuresJITCache() ? jitcacheReadSection(key, name) : null;
    }

    // Little-endian fields of an ArrayBuffer, or of the bytes a typed array such as a Uint8Array covers, each read at a
    // byte offset from their start.
    function byteReader(bytes) {
        const view = ArrayBuffer.isView(bytes) ? new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength) : new DataView(bytes);
        return {
            u8: offset => view.getUint8(offset),
            u16: offset => view.getUint16(offset, true),
            u32: offset => view.getUint32(offset, true),
            i32: offset => view.getInt32(offset, true),
            f32: offset => view.getFloat32(offset, true),
            u64: offset => view.getBigUint64(offset, true),
            i64: offset => view.getBigInt64(offset, true),
        };
    }

    return Object.freeze({
        role, sequence, scratchPath, fail, check, checkSame, status, delta, bodyEvents, readSection, byteReader,
    });
})();
