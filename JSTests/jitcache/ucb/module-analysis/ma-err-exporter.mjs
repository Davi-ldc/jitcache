export const maPresent = "present";

// Called often enough to compile to baseline code, so an importing run has a body to install.
export function maErrHot(i)
{
    return i * 5 + 1;
}
