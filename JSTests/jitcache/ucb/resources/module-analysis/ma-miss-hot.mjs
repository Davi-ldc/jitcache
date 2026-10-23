// Called often enough to compile to baseline code, so every importing run of module-analysis-miss.mjs has a body to
// install, under the delta capture compiled too.
export function maMissHot(i)
{
    return (i << 2) - i;
}
