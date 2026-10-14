// kill-points.js's checker for a kill before the first commit's root: the pack lists no body, and no temporary is left.
import { checkPackState } from "./temporaries.ts";

checkPackState({ bodies: 0, retired: false, temporaries: 0 });
