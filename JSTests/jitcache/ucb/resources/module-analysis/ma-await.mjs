// Two modules with top-level awaits, neither of which waits for the other: each logs when it starts and when it settles.
import { maLog } from "./ma-log.mjs";
import { maFirst } from "./ma-await-first.mjs";
import { maSecond } from "./ma-await-second.mjs";

maLog.push("joined");
export const maSettled = maLog.slice();
export const maSum = maFirst + maSecond;
