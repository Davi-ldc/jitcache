// The integer arithmetic the pin comparison reads operands with and folds constants by, on bigints at an operation's
// width.

// The top of user space on both architectures: a value from here up, such as a JSValue tag, stays literal.
export const userSpaceHigh = 1n << 48n;
// No heap, VM, executable-pool or structure-reservation object lies below 4 GiB on either architecture, natively or
// under QEMU, while Bun's non-PIE jsc does, so a value under it that names no allocation and no symbol is an integer.
export const anonymousLow = 1n << 32n;

const widthMask = (width: number) => (1n << BigInt(width)) - 1n;
export const wrap = (value: bigint, width: number) => value & widthMask(width);

export function toSigned(value: bigint, width: number): bigint {
  const wrapped = wrap(value, width);
  return wrapped >> BigInt(width - 1) ? wrapped - (1n << BigInt(width)) : wrapped;
}

export function parseNumber(text: string): bigint | null {
  const match = /^(-)?(0x[0-9a-fA-F]+|\d+)$/.exec(text);
  if (!match) return null;
  const magnitude = BigInt(match[2]);
  return match[1] ? -magnitude : magnitude;
}

export const hex = (value: bigint) => (value < 0n ? `-0x${(-value).toString(16)}` : `0x${value.toString(16)}`);

function rotateRight(value: bigint, amount: number, width: number): bigint {
  const count = BigInt(((amount % width) + width) % width);
  const wrapped = wrap(value, width);
  return count ? wrap((wrapped >> count) | (wrapped << (BigInt(width) - count)), width) : wrapped;
}

// The operations canonical.ts's rule 2 folds, on a register's known constant and an immediate, at the operation's width.
export function fold(operation: string, left: bigint, right: bigint, width: number): bigint | null {
  switch (operation) {
    case "add":
      return wrap(left + right, width);
    case "sub":
      return wrap(left - right, width);
    case "and":
      return wrap(left & right, width);
    case "or":
    case "orr":
      return wrap(left | right, width);
    case "xor":
    case "eor":
      return wrap(left ^ right, width);
    case "ror":
      return rotateRight(left, Number(wrap(right, 8)), width);
    case "rol":
      return rotateRight(left, width - (Number(wrap(right, 8)) % width), width);
    default:
      return null;
  }
}
