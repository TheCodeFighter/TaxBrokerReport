import type { Language } from "./ui_texts";

/** A plain decimal number as the server writes it: digits, one point, digits, an optional minus. */
const plainDecimal = /^-?\d+\.\d+$/;

/**
 * A number as the page shows it: Slovenian writes the decimal comma (`1234,50`), English keeps the
 * point (`1234.50`). Display only: nothing the page sends or reads goes through this. Text that is
 * not a plain decimal number (an ISIN, a date, an ID, `1.2.3`, an empty text) comes back as it is.
 * No digit grouping in either language, like the inputs and the server.
 */
export function localizeNumber(text: string, language: Language): string {
  return language === "sl" && plainDecimal.test(text) ? text.replace(".", ",") : text;
}

/**
 * A plain decimal number rounded to two decimals, half away from zero, on the digits of the text:
 * `4.9000` becomes `4.90`, `-1.005` becomes `-1.01`, `2` becomes `2.00`. Display only (fees and
 * benefits); no binary floating point. Text that is not a plain number comes back as it is.
 */
export function roundToCents(text: string): string {
  const match = /^(-?)(\d+)(?:\.(\d+))?$/.exec(text);

  if (match === null) {
    return text;
  }

  const sign = match[1] ?? "";
  const whole = match[2] ?? "";
  const fraction = (match[3] ?? "").padEnd(3, "0");
  const roundUp = fraction.charCodeAt(2) >= "5".charCodeAt(0);
  const cents = BigInt(whole + fraction.slice(0, 2)) + (roundUp ? 1n : 0n);
  const digits = cents.toString().padStart(3, "0");
  const rounded = `${digits.slice(0, -2)}.${digits.slice(-2)}`;

  return cents === 0n ? rounded : `${sign}${rounded}`;
}
