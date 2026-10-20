// with-scope.js requires this CommonJS module with Node's compile cache on, so that every run after the first decodes it
// from the payload the first run persisted.
function withScopeSquare(x)
{
    return (x * x) | 0;
}

function withScopeSum(count)
{
    let total = 0;
    for (let i = 0; i < count; ++i)
        total = (total + withScopeSquare(i)) | 0;
    return total;
}

module.exports = { withScopeSquare, withScopeSum };
