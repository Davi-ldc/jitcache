// atom-constants.js loads this CommonJS module without Node's compile cache in its Producer, so its bodies are captured
// from generated UCBs. Each function compares its argument with a string constant longer than an inline string; alpha
// is also a property name below.
function acaEqualsAlpha(s)
{
    return s === "atom-constants-a-alpha-long";
}

function acaNotEqualsBeta(s)
{
    return s !== "atom-constants-a-beta-long";
}

function acaBranchGamma(s)
{
    if ("atom-constants-a-gamma-long" === s)
        return 1;
    return 0;
}

function acaKeyedAlpha(o)
{
    return o["atom-constants-a-alpha-long"];
}

module.exports = { acaEqualsAlpha, acaNotEqualsBeta, acaBranchGamma, acaKeyedAlpha };
