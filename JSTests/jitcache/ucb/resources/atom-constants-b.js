// atom-constants.js loads this CommonJS module with Node's compile cache in its Producer, so Bun decodes it from the
// payload an earlier run persisted and its bodies are captured from decoded UCBs. Each function compares its argument
// with a string constant longer than an inline string, which the decode leaves a plain string. acbDelta also uses its
// constant as a property key when asked to, which makes that constant an atom in place; the Producer asks only after
// the function has compiled.
function acbEqualsAlpha(s)
{
    return s === "atom-constants-b-alpha-long";
}

function acbNotEqualsBeta(s)
{
    return s !== "atom-constants-b-beta-long";
}

// The comparison and `key` share one constant register, since generation makes one constant per string. o[key] reads
// through that constant's own value (get_by_val), whose property key makes its JSString an atom in place (SPEC-ucb.md
// F19); o["..."] would compile to get_by_id with an identifier of its own, which leaves the constant as it is, and
// could even make the decode give the constant its atom (F19). No other identifier of this module holds these
// characters. `key` is a let: Bun's runtime transpiler inlines a const whose value is a literal (const_values in its
// parser, with inlining on for target bun), which would turn o[key] back into o["..."].
function acbDelta(s, o)
{
    let key = "atom-constants-b-delta-long";
    if (o)
        return o[key];
    return s === "atom-constants-b-delta-long";
}

module.exports = { acbEqualsAlpha, acbNotEqualsBeta, acbDelta };
