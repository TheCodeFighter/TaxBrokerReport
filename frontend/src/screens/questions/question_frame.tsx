import type { ComponentChildren } from "preact";
import { useState } from "preact/hooks";

import { sendAnswers, type AnswerOutcome, type Rejection } from "../../api/answers";
import type { ApiClient } from "../../api/client";
import { SessionEndedError } from "../../api/client";
import { renderDiagnostic, type MessageCatalogue } from "../../i18n/messages";
import { uiText, type Language } from "../../i18n/ui_texts";
import {
  CANCEL_ANSWER,
  questionAnchor,
  rejectionTargets,
  type AnswerValue,
  type DecisionRequest,
  type RejectionTarget,
} from "../../run/questions";
import type { RunResult } from "../../run/submit";

/** What every question and answer list needs from the result screen. */
export interface QuestionEnv {
  language: Language;
  catalogue: MessageCatalogue | undefined;
  apiFetch: ApiClient["apiFetch"];
  /** The revision of the shown result; the server refuses an answer to any other. */
  revision: number;
  /** True while the shown result is out of date: nothing is sent. */
  disabled: boolean;
  /** The server accepted the call: show this result. */
  onResult: (result: RunResult) => void;
  /** The question or the revision is out of date: load the run again. */
  onStale: () => void;
  /** The server has no run any more. */
  onNoRun: () => void;
}

/**
 * Handles the outcome of an answer or delete call the same way everywhere. A rejection and a
 * failure are given back to the caller, which shows them beside its own inputs.
 */
export function applyOutcome(
  env: QuestionEnv,
  outcome: AnswerOutcome,
  onRejected: (rejections: Rejection[]) => void,
  onFailed: () => void,
): void {
  switch (outcome.kind) {
    case "result":
      env.onResult(outcome.result);
      return;
    case "stale":
      env.onStale();
      return;
    case "noRun":
      env.onNoRun();
      return;
    case "rejected":
      onRejected(outcome.rejections);
      return;
    case "failed":
      onFailed();
      return;
  }
}

/** The text of a rejection from the message catalogue, in the language of the page. */
export function rejectionText(env: QuestionEnv, rejection: Rejection): string {
  if (env.catalogue === undefined) {
    return uiText(env.language, "runErrorUnexpected");
  }

  return renderDiagnostic(env.catalogue, rejection.messageKey, rejection.messageParams, env.language);
}

interface FrameProps {
  env: QuestionEnv;
  request: DecisionRequest;
  title: string;
  /** The answer to send; `undefined` while the form is not complete, which disables the button. */
  answer: AnswerValue | undefined;
  /** The inputs; `messagesFor` gives the rejections that mark the given inputs. */
  children: (messagesFor: (targets: RejectionTarget[]) => ComponentChildren) => ComponentChildren;
}

/**
 * One open question: its heading (the target of the links), the inputs, "Send answer" and "Cancel
 * the question" with what a cancel does. The form is disabled while a call runs and while the
 * result is out of date. Nothing is sent until the user presses a button.
 */
export function QuestionFrame({ env, request, title, answer, children }: FrameProps) {
  const [busy, setBusy] = useState(false);
  const [rejections, setRejections] = useState<Rejection[]>([]);
  const [problem, setProblem] = useState(false);
  const [confirmCancel, setConfirmCancel] = useState(false);
  const anchor = questionAnchor(request.id);

  const post = async (value: AnswerValue) => {
    setBusy(true);
    setRejections([]);
    setProblem(false);

    try {
      const outcome = await sendAnswers(env.apiFetch, env.revision, [{ requestId: request.id, answer: value }]);

      applyOutcome(env, outcome, setRejections, () => setProblem(true));
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setProblem(true);
      }
    } finally {
      setBusy(false);
      setConfirmCancel(false);
    }
  };

  const messagesFor = (targets: RejectionTarget[]) =>
    rejections
      .filter((rejection) =>
        rejectionTargets(rejection.messageKey, rejection.messageParams).some((target) => targets.includes(target)),
      )
      .map((rejection, index) => (
        <p key={index} class="field-error" role="alert">
          {rejectionText(env, rejection)}
        </p>
      ));

  return (
    <section class="question" id={anchor} aria-labelledby={`${anchor}-title`}>
      <h5 id={`${anchor}-title`} tabIndex={-1}>
        {title}
      </h5>
      <fieldset class="question-fields" disabled={busy || env.disabled}>
        {children(messagesFor)}
        <div class="question-actions">
          <button type="button" class="button" disabled={answer === undefined} onClick={() => answer !== undefined && void post(answer)}>
            {uiText(env.language, busy ? "questionSending" : "questionSend")}
          </button>
          <button type="button" class="button button-secondary" onClick={() => setConfirmCancel(true)}>
            {uiText(env.language, "questionCancel")}
          </button>
        </div>
        {confirmCancel ? (
          <div class="notice">
            <p>{uiText(env.language, "questionCancelWhat")}</p>
            <div class="question-actions">
              <button type="button" class="button" onClick={() => void post(CANCEL_ANSWER)}>
                {uiText(env.language, "questionCancelConfirm")}
              </button>
              <button type="button" class="button button-secondary" onClick={() => setConfirmCancel(false)}>
                {uiText(env.language, "questionCancelKeep")}
              </button>
            </div>
          </div>
        ) : null}
        {messagesFor(["form"])}
        <div role="alert">{problem ? <p class="error">{uiText(env.language, "runErrorUnexpected")}</p> : null}</div>
      </fieldset>
    </section>
  );
}
