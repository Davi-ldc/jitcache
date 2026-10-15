// producer-kill.js's checker for the compact at ratio 0 after each kill, which removes the bytes past the pack's valid end
// and the dead bytes and evicts nothing.
import { checkCompactCleanup } from "./checker.ts";

checkCompactCleanup();
