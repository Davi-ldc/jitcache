// The module of decoded-code-bun.js, which that script loads from disk under Node's compile cache and its app requires
// from beside it in a `bun build --compile` executable. DcbPoint is an ordinary function that `new` reaches first: such
// an executable built with --bytecode caches its call body alone, as eager generation for bytecode caches leaves it.
// dcbCounter is a generator, so that the app can see its body's code returned to the payload it was decoded from.
function dcbSum(x)
{
    function dcbChild(y)
    {
        return y + 1;
    }
    return dcbChild(x) * 2;
}

function DcbPoint(x)
{
    if (new.target) {
        this.x = x;
        return;
    }
    return x * 3;
}

function* dcbCounter(limit)
{
    for (let i = 0; i < limit; ++i)
        yield i;
}

module.exports = { dcbSum, DcbPoint, dcbCounter };
