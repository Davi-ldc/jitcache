// The module of decoded-slots-bun-app.js, built beside it with `bun build --compile --bytecode`. The app requires it
// twice at run time, and the second evaluation gives dbC's decoded call slot the later request that SPEC-ucb.md
// section 13.3 asks of it (see the app).
//
// dbC's call body holds the one string constant of the executable with these characters. A `--compile --bytecode` build
// keeps every string of four or more characters in one table that all its chunks share, and decodes an identifier's
// entry as an atom, which a constant with the same characters then shares (DecoderStringTable::atomFor and jsStringFor
// in runtime/CachedTypes.cpp). So no identifier or other constant of the executable holds them: o[key] reads through
// the constant's own value (get_by_val), where o["..."] would compile to get_by_id with an identifier of its own, and
// the app builds its object keys at run time. `key` is a let, which Bun's transpiler never inlines, where it inlines a
// const whose value is a literal whenever it minifies syntax.
function dbC(x, o)
{
    if (new.target) {
        this.value = x;
        return;
    }
    let key = "decoded-slots-bun-long-key";
    if (o)
        return o[key];
    return x === key;
}

module.exports = { dbC };
