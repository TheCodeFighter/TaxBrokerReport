/**
 * True when the text is a Slovenian tax number: eight digits whose last digit is the check digit
 * of the first seven (weights 8 to 2, 11 minus the sum modulo 11, 10 written as 0). A sum that is
 * a multiple of 11 has no check digit. The server makes the same check and only warns; this one
 * only drives a hint under the input and never blocks.
 */
export function isSlovenianTaxNumber(text: string): boolean {
  if (!/^[0-9]{8}$/.test(text)) {
    return false;
  }

  let sum = 0;

  for (let position = 0; position < 7; position += 1) {
    sum += (8 - position) * Number(text[position]);
  }

  const remainder = sum % 11;

  if (remainder === 0) {
    return false;
  }

  const check = 11 - remainder === 10 ? 0 : 11 - remainder;

  return check === Number(text[7]);
}

/** True when the typed tax number has eight digits but a wrong check digit (the hint is shown). */
export function hasWrongCheckDigit(typed: string): boolean {
  const text = typed.trim();

  return /^[0-9]{8}$/.test(text) && !isSlovenianTaxNumber(text);
}
