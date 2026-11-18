import { useState } from "preact/hooks";

import type { AnswerOutcome, Rejection } from "../../api/answers";
import { SessionEndedError } from "../../api/client";
import { uiText } from "../../i18n/ui_texts";
import { cancelDeveloperExport, confirmDeveloperExport } from "../../run/developer_export";
import { questionAnchor, type DecisionRequest } from "../../run/questions";
import { applyOutcome, rejectionText, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/**
 * The one confirmation of the unsafe developer export: a red warning with every affected
 * investment and the risk, then Confirm or Cancel. Confirm sends the three confirmations of the
 * service one after the other; Cancel goes back to the failure question. Nothing is sent until the
 * user presses a button.
 */
export function DeveloperConfirmation({ env, request }: Props) {
  const [busy, setBusy] = useState(false);
  const [rejections, setRejections] = useState<Rejection[]>([]);
  const [problem, setProblem] = useState(false);
  const { language } = env;
  const anchor = questionAnchor(request.id);
  const investments = request.investments ?? [];

  const run = async (send: () => Promise<AnswerOutcome>) => {
    setBusy(true);
    setRejections([]);
    setProblem(false);

    try {
      applyOutcome(env, await send(), setRejections, () => setProblem(true));
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setProblem(true);
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <section class="question danger-panel" id={anchor} aria-labelledby={`${anchor}-title`} role="alert">
      <h5 id={`${anchor}-title`} tabIndex={-1}>
        {uiText(language, "developerConfirmTitle")}
      </h5>
      <fieldset class="question-fields" disabled={busy || env.disabled}>
        <p>{uiText(language, "developerConfirmWarning")}</p>
        {investments.length === 0 ? null : (
          <>
            <h6>{uiText(language, "developerConfirmInvestments")}</h6>
            <ul class="plain-list">
              {investments.map((investment) => (
                <li key={investment.isin}>{`${investment.name} (${investment.isin})`}</li>
              ))}
            </ul>
          </>
        )}
        <p class="danger-notice">{uiText(language, "developerConfirmRisk")}</p>
        <div class="question-actions">
          <button
            type="button"
            class="button button-danger"
            onClick={() => void run(() => confirmDeveloperExport(env.apiFetch, env.revision, request))}
          >
            {uiText(language, busy ? "questionSending" : "developerConfirmButton")}
          </button>
          <button
            type="button"
            class="button button-secondary"
            onClick={() => void run(() => cancelDeveloperExport(env.apiFetch, env.revision, request))}
          >
            {uiText(language, "developerCancelButton")}
          </button>
        </div>
        <p class="hint">{uiText(language, "developerCancelHint")}</p>
        {rejections.map((rejection, index) => (
          <p key={index} class="field-error" role="alert">
            {rejectionText(env, rejection)}
          </p>
        ))}
        <div role="alert">{problem ? <p class="error">{uiText(language, "runErrorUnexpected")}</p> : null}</div>
      </fieldset>
    </section>
  );
}
