// With ma-uncompiled-b.mjs, a module whose top level runs once and never compiles.
import { maUncompiledB } from "./ma-uncompiled-b.mjs";

export const maUncompiledA = `a after ${maUncompiledB}`;
