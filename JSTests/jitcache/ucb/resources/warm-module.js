// A module an app embeds beside its entry module and loads first, through the same loader path as the module it tests,
// so that the loader's first run, which creates and runs some of Bun's own builtins for the first time, happens outside
// the app's statistics windows. Bun hashes such a builtin's root from its text, and records the body it generates.
module.exports = { warm: true };
