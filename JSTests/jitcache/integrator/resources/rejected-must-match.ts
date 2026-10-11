// must-match.js's checker for its Consumer: start found the Producer's header incompatible at the must-match option the
// Producer set.
import { checkStartResult } from "./checker.ts";

checkStartResult("rejected", "start.incompatible", ["evalMode"]);
