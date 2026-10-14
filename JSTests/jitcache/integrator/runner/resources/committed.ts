// kill-points.js's checker for a kill right after the first commit's root: the pack lists that body.
import { checkPackState } from "./temporaries.ts";

checkPackState({ bodies: 1, retired: false, temporaries: 0 });
