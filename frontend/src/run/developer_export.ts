import { sendAnswers, type AnswerOutcome } from "../api/answers";
import type { ApiClient } from "../api/client";
import { CANCEL_ANSWER, type DecisionRequest } from "./questions";
import type { RunResult } from "./submit";

/** The request type of the three confirmation steps of the developer export. */
export const DEVELOPER_CONFIRMATION_TYPE = "developer_export_confirmation_required";

/** The one answer of a confirmation step. */
export const CONFIRM_ANSWER = "confirm";

/** The service asks three confirmations, one after the other; the page sends them after one click. */
const LAST_STEP = 3;

/** The ISINs a confirmation step lists, in the order of the request. */
export function developerIsins(request: DecisionRequest): string[] {
  return (request.investments ?? []).map((investment) => investment.isin);
}

/**
 * The open confirmation step of a result, only when it lists exactly the given ISINs. A different
 * list means the question changed in between, so nothing is confirmed for it by the page.
 */
export function nextDeveloperStep(result: RunResult, isins: string[]): DecisionRequest | undefined {
  const step = result.requests.find((request) => request.type === DEVELOPER_CONFIRMATION_TYPE);

  if (step === undefined) {
    return undefined;
  }

  const listed = developerIsins(step);

  return listed.length === isins.length && listed.every((isin, index) => isin === isins[index]) ? step : undefined;
}

/**
 * The one confirmation of the page: confirms the open step, then every following step that lists
 * the same ISINs, up to step 3. It stops at the first answer that is not a result and gives that
 * outcome back; otherwise it gives back the last result (the final one, or the one that has the
 * next step missing or changed). One call is made for each step, each with the new revision.
 */
export async function confirmDeveloperExport(
  apiFetch: ApiClient["apiFetch"],
  revision: number,
  request: DecisionRequest,
): Promise<AnswerOutcome> {
  const isins = developerIsins(request);
  let outcome = await sendAnswers(apiFetch, revision, [{ requestId: request.id, answer: CONFIRM_ANSWER }]);
  let step = request.step ?? 1;

  while (outcome.kind === "result" && step < LAST_STEP) {
    const next = nextDeveloperStep(outcome.result, isins);

    if (next === undefined) {
      break;
    }

    step = next.step ?? step + 1;
    outcome = await sendAnswers(apiFetch, outcome.result.runRevision, [{ requestId: next.id, answer: CONFIRM_ANSWER }]);
  }

  return outcome;
}

/** Cancels the open step; the service then asks the failure question again. */
export function cancelDeveloperExport(
  apiFetch: ApiClient["apiFetch"],
  revision: number,
  request: DecisionRequest,
): Promise<AnswerOutcome> {
  return sendAnswers(apiFetch, revision, [{ requestId: request.id, answer: CANCEL_ANSWER }]);
}
