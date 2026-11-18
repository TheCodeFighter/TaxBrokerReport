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
