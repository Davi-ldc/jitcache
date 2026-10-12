// A module embedded beside supplied-digests-app.js in the standalone executable, which the app loads at run time, so the
// executable records its source digest. Its source is CommonJS. A CommonJS build, with --bytecode or with --format=cjs,
// keeps it CommonJS, so require evaluates it through JSCommonJSModule::evaluate. Where the app overrides the module wrapper
// first, Bun evaluates the module's text inside the other wrapper, and the provider no longer vouches for the recorded
// digest, which JSCommonJSModule::evaluate clears (SPEC-ucb.md section 7.2.6). A build without either flag turns it into
// an ES module, which the module loader evaluates.
function sdModuleHot(x)
{
    let total = x;
    for (let i = 0; i < 4; ++i)
        total = (total * 13 + i) | 0;
    return total;
}

module.exports = { sdModuleHot };
