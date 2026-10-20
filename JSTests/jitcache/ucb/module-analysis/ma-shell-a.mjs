// With ma-shell-b.mjs, the modules module-analysis-shell.mjs imports in the jsc shell.
import { maShellB } from "./ma-shell-b.mjs";

export const maShellA = `a after ${maShellB}`;
export default function maShellDefault()
{
    return import.meta.url.length > 0;
}
