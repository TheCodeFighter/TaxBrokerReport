/** Units have 8 decimals; the page counts them as whole ticks of 10^-8 units, exactly (BigInt). */
const TICKS_PER_UNIT = 100_000_000n;
const UNIT_DECIMALS = 8;

/** A reduced number above this in a ratio is unusual and worth a second look. */
const UNUSUAL_RATIO_LIMIT = 1000;

/** The largest whole number the page takes for a ratio: 15 digits always fit a safe integer. */
const MAX_WHOLE_DIGITS = 15;

/** An exact amount as two whole numbers in strings, as the backend sends an EUR fraction. */
export interface Fraction {
  numerator: string;
  denominator: string;
}

/** The two whole numbers of a split: `newShares` new units for `oldShares` old ones. */
export interface Ratio {
  newShares: number;
  oldShares: number;
}

/**
 * A quantity above or equal to 0 with up to 8 decimals, in ticks: `0,52` and `0.52` are
 * 52000000. The separator is a comma or a point, never both, and there is no digit grouping:
 * `1,234` is 1.234 and `1.000,5` is refused. Empty, signed or longer text gives `undefined`.
 */
export function parseUnits(text: string): bigint | undefined {
  const match = /^(\d+)(?:[.,](\d{1,8}))?$/.exec(text.trim());

  if (match === null) {
    return undefined;
  }

  const whole = BigInt(match[1] as string);
  const decimals = (match[2] ?? "").padEnd(UNIT_DECIMALS, "0");

  return whole * TICKS_PER_UNIT + BigInt(decimals);
}

/** The ticks as a quantity with all 8 decimals, like the backend: 520000000 is `5.20000000`. */
export function formatUnits(ticks: bigint): string {
  const whole = ticks / TICKS_PER_UNIT;
  const decimals = (ticks % TICKS_PER_UNIT).toString().padStart(UNIT_DECIMALS, "0");

  return `${whole}.${decimals}`;
}

/** A whole number without sign, grouping or decimals, at most 15 digits; `undefined` otherwise. */
export function parseWholeNumber(text: string): number | undefined {
  const trimmed = text.trim();

  if (!new RegExp(`^\\d{1,${MAX_WHOLE_DIGITS}}$`).test(trimmed)) {
    return undefined;
  }

  return Number(trimmed);
}

function gcd(first: bigint, second: bigint): bigint {
  let a = first;
  let b = second;

  while (b !== 0n) {
    [a, b] = [b, a % b];
  }

  return a;
}

/**
 * The ratio reduced by the greatest common divisor: 20 new for 10 old is 2 / 1. `undefined` when
 * a number is not above 0, both are equal (the position would not change) or a reduced number is
 * not a safe integer.
 */
export function reduceRatio(newShares: bigint | number, oldShares: bigint | number): Ratio | undefined {
  const newer = BigInt(newShares);
  const older = BigInt(oldShares);

  if (newer <= 0n || older <= 0n || newer === older) {
    return undefined;
  }

  const divisor = gcd(newer, older);
  const reducedNew = newer / divisor;
  const reducedOld = older / divisor;

  if (reducedNew > BigInt(Number.MAX_SAFE_INTEGER) || reducedOld > BigInt(Number.MAX_SAFE_INTEGER)) {
    return undefined;
  }

  return { newShares: Number(reducedNew), oldShares: Number(reducedOld) };
}

/** The ratio from the shares held before and after the action: 0.52 before, 5.20 after is 10 / 1. */
export function ratioFromShareCounts(before: string, after: string): Ratio | undefined {
  const beforeTicks = parseUnits(before);
  const afterTicks = parseUnits(after);

  if (beforeTicks === undefined || afterTicks === undefined) {
    return undefined;
  }

  return reduceRatio(afterTicks, beforeTicks);
}

/** True when either reduced number is above 1000, for example 520000001 / 52000000. */
export function ratioLooksUnusual(newShares: number, oldShares: number): boolean {
  return newShares > UNUSUAL_RATIO_LIMIT || oldShares > UNUSUAL_RATIO_LIMIT;
}

/**
 * The position after the split: position × new / old, exact, rounded once to 8 decimals with
 * halves away from zero, as the backend does. `undefined` when the position is not a quantity or
 * the result is not above 0.
 */
export function previewUnits(position: string, newShares: number, oldShares: number): string | undefined {
  const ticks = parseUnits(position);

  if (ticks === undefined || newShares <= 0 || oldShares <= 0) {
    return undefined;
  }

  const numerator = ticks * BigInt(newShares);
  const denominator = BigInt(oldShares);
  // Halves round up for a positive value, which is away from zero.
  const rounded = (2n * numerator + denominator) / (2n * denominator);

  return rounded === 0n ? undefined : formatUnits(rounded);
}

/** An EUR fraction rounded half away from zero to 4 decimals: 1/3 is `0.3333`. Display only. */
export function fractionToEur4(fraction: Fraction): string {
  return eur4(BigInt(fraction.numerator), BigInt(fraction.denominator));
}

/** Rounds half away from zero; a negative amount is rounded as its positive and signed again. */
function eur4(numerator: bigint, denominator: bigint): string {
  const scale = 10_000n;
  const magnitude = numerator < 0n ? -numerator : numerator;
  const rounded = (2n * magnitude * scale + denominator) / (2n * denominator);
  const decimals = (rounded % scale).toString().padStart(4, "0");
  const sign = numerator < 0n && rounded !== 0n ? "-" : "";

  return `${sign}${rounded / scale}.${decimals}`;
}

/**
 * The value of the removed ticks at the unit value of the question, in EUR with 4 decimals:
 * 2 ticks at EUR 25 are 5e-7, which shows as `0.0000`.
 */
export function removedValueEur(unitValue: Fraction, removedTicks: bigint): string {
  return eur4(removedTicks * BigInt(unitValue.numerator), TICKS_PER_UNIT * BigInt(unitValue.denominator));
}

/**
 * The rounding budget used after the entered correction: the budget already used plus the value
 * of the removed ticks, in EUR with 4 decimals. The backend decides; this is only shown.
 */
export function budgetAfterEur(budgetUsed: Fraction, unitValue: Fraction, removedTicks: bigint): string {
  const usedDenominator = BigInt(budgetUsed.denominator);
  const valueDenominator = TICKS_PER_UNIT * BigInt(unitValue.denominator);
  const numerator =
    BigInt(budgetUsed.numerator) * valueDenominator + removedTicks * BigInt(unitValue.numerator) * usedDenominator;

  return eur4(numerator, usedDenominator * valueDenominator);
}
