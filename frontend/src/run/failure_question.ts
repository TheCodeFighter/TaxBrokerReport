/** The two answers of a failure question the page offers; the server may list a third (4.22). */
export const EXCLUDE_ANSWER = "exclude_failed_instruments";
export const FAIL_ANSWER = "fail_report";

const forms = new Map<string, string>([
  ["capital_gains_failure_resolution_required", "Doh-KDVP"],
  ["dividends_failure_resolution_required", "Doh-Div"],
  ["other_interest_failure_resolution_required", "Doh-Obr"],
]);

/** The form code a failure question belongs to; `undefined` for any other type. */
export function failureForm(type: string): string | undefined {
  return forms.get(type);
}

/**
 * The choices the page shows: the two normal words, in this order, when the request offers them.
 * `developer_export` is never shown here; the three confirmations of 4.22 add it.
 */
export function failureChoices(options: string[] | undefined): string[] {
  return [EXCLUDE_ANSWER, FAIL_ANSWER].filter((word) => (options ?? []).includes(word));
}
