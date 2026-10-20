// Called often enough to compile to baseline code, so an importing run has a body to install.
export function maHot(i)
{
    return (i * 3) ^ (i >> 1);
}
