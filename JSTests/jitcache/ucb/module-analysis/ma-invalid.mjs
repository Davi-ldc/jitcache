// The module module-analysis-invalid.mjs damages the analysis of. It imports nothing and exports two names of one length,
// so its analysis is 36 bytes laid out the same whichever name comes first: the string table (W 1, n 2, the offsets 0, 3
// and 6, a zero byte, then the blob from byte 12), then the body from byte 18 (flags, V 1, q 0 at 22, r 2 at 26, the
// record tags 04 04 at 30, the ids 00 00 01 01 at 32).
export const ka = 1;
export const kb = 2;
