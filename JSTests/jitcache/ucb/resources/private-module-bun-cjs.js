// The CommonJS module of private-module-bun.js, which the host requires and a Bun.ModuleGraph with `globals` imports
// again; the graph compiles its wrapper on a GraphCommonJSSourceProvider over the module's provider. In the app, a
// `bun build --compile --format=cjs` executable, it sits beside the entry module and keeps the source digest the build
// recorded.
let pmbCalls = 0;

function pmbCount(x)
{
    ++pmbCalls;
    return (x * 7 + (typeof pmbBase === "number" ? pmbBase : 1)) | 0;
}

module.exports = { pmbCount, calls: () => pmbCalls };
