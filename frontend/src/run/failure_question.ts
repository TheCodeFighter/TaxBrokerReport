/** The answers of a failure question the page offers; the server lists the third only in developer mode. */
export const EXCLUDE_ANSWER = "exclude_failed_instruments";
export const FAIL_ANSWER = "fail_report";
export const DEVELOPER_ANSWER = "developer_export";

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
 * The choices the page shows: the two normal words, then `developer_export` last, each only when
 * the request offers it. Choosing the developer export is not the confirmation: the confirmation
 * screen (`developer_export.ts`) comes next.
 */
export function failureChoices(options: string[] | undefined): string[] {
  return [EXCLUDE_ANSWER, FAIL_ANSWER, DEVELOPER_ANSWER].filter((word) => (options ?? []).includes(word));
}
