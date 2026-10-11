// default-mode.js's checker for the runs of its strict sequence: the run started and checked strictly.
import { checkFinalStatus } from "./checker.ts";

checkFinalStatus({ state: "started", strict: true });
