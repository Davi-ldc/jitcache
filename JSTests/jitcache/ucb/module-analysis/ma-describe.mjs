// What the module-analysis tests print for a value: a namespace or an object as its keys and values, in the order its
// keys come, an array as JSON, a function as its name.
export function maDescribe(value, depth = 0)
{
    if (Array.isArray(value))
        return JSON.stringify(value);
    if (typeof value === "function")
        return `function ${value.name}`;
    if (value !== null && typeof value === "object") {
        if (depth >= 2)
            return "{ ... }";
        return `{ ${Object.keys(value).map(key => `${key}: ${maDescribe(value[key], depth + 1)}`).join(", ")} }`;
    }
    return JSON.stringify(value);
}

// The lines module-analysis.mjs and its worker print for the graph's root namespace: every export, the wide module's
// names counted and summed, import.meta.url relative to `base`, and the hot function's total.
export function maDescribeRoot(root, base, relative)
{
    const lines = [];
    for (const key of Object.keys(root)) {
        const value = root[key];
        if (key === "maWide") {
            const names = Object.keys(value);
            lines.push(`maWide: ${names.length} names, sum ${names.reduce((sum, name) => sum + value[name], 0)}`);
        } else if (key === "maMetaURL")
            lines.push(`maMetaURL: ${relative(base, value)}`);
        else if (key === "maHot") {
            let total = 0;
            for (let i = 0; i < 1000; ++i)
                total = (total + value(i)) | 0;
            lines.push(`maHot: ${total}`);
        } else
            lines.push(`${key}: ${maDescribe(value)}`);
    }
    lines.push(`maNamedFunction(): ${root.maNamedFunction()}`);
    lines.push(`MaAnonymousClass.kind(): ${root.MaAnonymousClass.kind()}`);
    return lines;
}
