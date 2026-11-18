import type { MessageParams } from "../i18n/messages";
import type { AnswerValue } from "../run/questions";
import type { RunResult } from "../run/submit";
import type { ApiClient } from "./client";

/** One answer to one open question. */
export interface AnswerItem {
  requestId: string;
  answer: AnswerValue;
}

/** One answer the server refused: its place in the call, its message key and parameters. */
export interface Rejection {
  index: number;
  messageKey: string;
  messageParams: MessageParams;
}

/** What an answer or delete call did. */
export type AnswerOutcome =
  /** A 200: the new result, with a new revision. */
  | { kind: "result"; result: RunResult }
  /** A 422: the rejections to show beside the inputs they mark. */
  | { kind: "rejected"; rejections: Rejection[] }
  /** The question or the revision is out of date: the page loads the run again. */
  | { kind: "stale" }
  /** A 404: the server has no run (it was restarted). */
  | { kind: "noRun" }
  /** Any other answer, for example a 500. */
  | { kind: "failed" };

const JSON_HEADERS = { "Content-Type": "application/json" };

// The message keys that say the question or the answer is out of date; the page loads the run again.
const staleKeys = new Set([
  "answer_rejected.stale_revision",
  "answer_rejected.unknown_request",
  "answer_rejected.stale_revision_delete",
  "answer_rejected.unknown_decision",
]);

async function sortAnswer(response: Response): Promise<AnswerOutcome> {
  if (response.status === 200) {
    return { kind: "result", result: (await response.json()) as RunResult };
  }

  if (response.status === 404) {
    return { kind: "noRun" };
  }

  if (response.status === 422) {
    try {
      const body = (await response.json()) as { rejections?: Rejection[] };
      const rejections = body.rejections ?? [];

      if (rejections.length === 0) {
        return { kind: "failed" };
      }

      return rejections.some((rejection) => staleKeys.has(rejection.messageKey))
        ? { kind: "stale" }
        : { kind: "rejected", rejections };
    } catch {
      return { kind: "failed" };
    }
  }

  return { kind: "failed" };
}

/**
 * Sends answers for the result of `revision`. A lost session still throws from `apiFetch`, so the
 * page ends it.
 */
export async function sendAnswers(
  apiFetch: ApiClient["apiFetch"],
  revision: number,
  answers: AnswerItem[],
): Promise<AnswerOutcome> {
  const response = await apiFetch("/api/run/answers", {
    method: "POST",
    headers: JSON_HEADERS,
    body: JSON.stringify({ runRevision: revision, answers }),
  });

  return sortAnswer(response);
}

/** Deletes recorded answers by ID, or all of them, for the result of `revision`. */
export async function deleteDecisions(
  apiFetch: ApiClient["apiFetch"],
  revision: number,
  decisionIds: string[] | "all",
): Promise<AnswerOutcome> {
  const body = decisionIds === "all" ? { runRevision: revision, all: true } : { runRevision: revision, decisionIds };
  const response = await apiFetch("/api/run/decisions/delete", {
    method: "POST",
    headers: JSON_HEADERS,
    body: JSON.stringify(body),
  });

  return sortAnswer(response);
}

/** The current result of the server, for when the shown one is out of date. */
export async function loadCurrentRun(apiFetch: ApiClient["apiFetch"]): Promise<AnswerOutcome> {
  const response = await apiFetch("/api/run");

  return sortAnswer(response);
}
