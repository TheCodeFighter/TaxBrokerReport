import type { UiTextKey } from "../i18n/ui_texts";
import { isCountryCode } from "./country_codes";
import type { AnswerValue } from "./questions";

/** The answer that withdraws an optional treaty claim. */
export const NO_CLAIM_ANSWER = "no_claim";

/** The answer that leaves a payer choice open without blocking the report. */
export const SKIP_ANSWER = "skip";

/** The income choice that needs a kind of its own. */
export const OTHER_INTEREST = "other_interest";

const choiceTexts = new Map<string, UiTextKey>([
  ["capital_gains", "choiceCapitalGains"],
  ["exclude_from_supported_reports", "choiceExclude"],
  ["dividends", "choiceDividends"],
  ["bank_deposit_interest", "choiceDepositInterest"],
  [OTHER_INTEREST, "choiceOtherInterest"],
  ["broker_cash", "choiceBrokerCash"],
  ["bond_coupon", "choiceBondCoupon"],
  ["ordinary", "choiceOrdinary"],
  ["discount_or_zero_coupon", "choiceDiscount"],
  ["above_limit", "choiceAboveLimit"],
  ["not_above_limit", "choiceNotAboveLimit"],
  ["exclude_failed_instruments", "choiceFailExclude"],
  ["fail_report", "choiceFailReport"],
  ["developer_export", "choiceFailDeveloper"],
]);

/** The text of a choice word; `undefined` for a word the page does not know (it is shown as it is). */
export function choiceText(word: string): UiTextKey | undefined {
  return choiceTexts.get(word);
}

/**
 * The answer of an income classification question. `other_interest` also needs its kind: until
 * both are chosen there is no answer. Nothing is preselected, so `undefined` is the start.
 */
export function incomeAnswer(choice: string | undefined, subtype: string | undefined): AnswerValue | undefined {
  if (choice === undefined) {
    return undefined;
  }

  if (choice !== OTHER_INTEREST) {
    return choice;
  }

  return subtype === undefined ? undefined : { choice, subtype };
}

/** The class labels worth showing: a row with an empty class column gives an empty label, left out. */
export function shownClassLabels(labels: string[]): string[] {
  return labels.filter((label) => label.trim() !== "");
}

/** The answer that picks the reference row at this position of the options. */
export function payerAnswer(position: number | undefined): AnswerValue | undefined {
  return position === undefined ? undefined : { option: position };
}

/** A typed country code as the server takes it (an assigned two-letter code in capitals), or `undefined`. */
export function typedCountry(input: string): string | undefined {
  const code = input.trim().toUpperCase();

  return isCountryCode(code) ? code : undefined;
}

/** Whether the typed text has two letters but is no country, so the user must change it. */
export function typedCountryIsWrong(input: string): boolean {
  const code = input.trim();

  return code.length >= 2 && typedCountry(code) === undefined;
}

/** The answer that gives this country code; the server checks that it is a country. */
export function countryAnswer(code: string | undefined): AnswerValue | undefined {
  return code === undefined ? undefined : { country: code };
}

/** What is wrong with a treaty text. */
export type TreatyProblem = "empty" | "tooLong" | "yesNo" | "characters";

/** The longest treaty text, in characters (code points), as the server counts. */
export const MAX_TREATY_LENGTH = 100;

/** A treaty text as the server takes it, or the first problem with it. */
export type TreatyText = { ok: true; text: string } | { ok: false; problem: TreatyProblem };

function isBlank(character: string | undefined): boolean {
  return character === " " || character === " ";
}

function trimBlanks(input: string): string {
  let start = 0;
  let end = input.length;

  while (start < end && isBlank(input[start])) {
    start += 1;
  }

  while (end > start && isBlank(input[end - 1])) {
    end -= 1;
  }

  return input.slice(start, end);
}

/** True for a control character or a lone surrogate, which the server refuses. */
function isUnusable(code: number): boolean {
  return code < 0x20 || (code >= 0x7f && code <= 0x9f) || (code >= 0xd800 && code <= 0xdfff);
}

/**
 * Checks a treaty text with the rule of the server (spaces and no-break spaces trimmed, 1 to 100
 * characters, no control characters, not the word true or false in any case), so the problem shows at
 * the field. The server checks it again.
 */
export function treatyText(input: string): TreatyText {
  const text = trimBlanks(input);
  const characters = Array.from(text);

  if (characters.some((character) => isUnusable(character.codePointAt(0) ?? 0))) {
    return { ok: false, problem: "characters" };
  }

  if (characters.length > MAX_TREATY_LENGTH) {
    return { ok: false, problem: "tooLong" };
  }

  if (text === "") {
    return { ok: false, problem: "empty" };
  }

  const lower = text.toLowerCase();

  if (lower === "true" || lower === "false") {
    return { ok: false, problem: "yesNo" };
  }

  return { ok: true, text };
}

/** The answer of a treaty text, or `undefined` while the text is not valid. */
export function treatyAnswer(input: string): AnswerValue | undefined {
  const checked = treatyText(input);

  return checked.ok ? { statement: checked.text } : undefined;
}
