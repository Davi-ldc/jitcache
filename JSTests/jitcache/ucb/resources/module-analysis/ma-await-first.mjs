import { maLog } from "./ma-log.mjs";

maLog.push("first starts");
await new Promise(resolve => setTimeout(resolve, 1));
maLog.push("first settles");
export const maFirst = 1;
