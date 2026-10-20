// The module module.mjs imports: a live binding it reads, two constants and a function that updates the binding, whose
// body is a child of this module's.
export let counter = 0;
export const label = "jitcache-module";
export const deepLabel = "jitcache-module-read-deep";
export function increment(step) {
    counter += step;
    return counter;
}
