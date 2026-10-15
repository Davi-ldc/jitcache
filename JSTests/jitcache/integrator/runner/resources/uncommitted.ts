// kill-points.js's checker for a kill before the first commit's root: the pack lists no body, its root still holds the
// new artifact's valid end, and no temporary is left.
import { checkPackState, emptyPackValidEnd } from "./temporaries.ts";

checkPackState({ bodies: 0, validEnd: emptyPackValidEnd, retired: false, temporaries: 0 });
