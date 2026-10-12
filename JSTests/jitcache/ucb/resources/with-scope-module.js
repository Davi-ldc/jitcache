// with-scope.js requires this CommonJS module with Node's compile cache on, so that later runs decode it from a payload
// Bun generated without the with-scope bit while their request carries it.
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
