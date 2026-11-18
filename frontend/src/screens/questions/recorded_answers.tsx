import { useState } from "preact/hooks";

import { deleteDecisions } from "../../api/answers";
import { SessionEndedError } from "../../api/client";
import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { decisionRows } from "../../run/decisions_view";
import type { Decision } from "../../run/questions";
import { applyOutcome, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  decisions: Decision[];
}

/**
 * The recorded answers with a delete for each and for all of them. A deleted answer brings its
 * question back with the next result. Nothing is sent while the result is out of date.
 */
export function RecordedAnswers({ env, decisions }: Props) {
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState(false);
  const { language } = env;

  if (decisions.length === 0) {
    return null;
  }

  const remove = async (ids: string[] | "all") => {
    setBusy(true);
    setProblem(false);

    try {
      const outcome = await deleteDecisions(env.apiFetch, env.revision, ids);

      // A rejection of a delete has no input to mark: it is shown as one problem line.
      applyOutcome(env, outcome, () => setProblem(true), () => setProblem(true));
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
    <section class="recorded-answers" aria-labelledby="recorded-answers-title">
      <h3 id="recorded-answers-title">{uiTextWith(language, "answersTitle", { count: decisions.length })}</h3>
      <p class="hint">{uiText(language, "answersHint")}</p>
      <ul class="plain-list">
        {decisionRows(decisions, language).map((row) => (
          <li key={row.id} class="recorded-answer">
            <div>
              <p>
                <strong>{row.title}</strong>
                {row.investment === undefined ? null : `: ${row.investment}`}
              </p>
              <p>{row.answer}</p>
              {row.details.length === 0 ? null : (
                <details>
                  <summary>{uiText(language, "answerListSummary")}</summary>
                  <ul class="plain-list">
                    {row.details.map((line, index) => (
                      <li key={index}>{line}</li>
                    ))}
                  </ul>
                </details>
              )}
              {row.notes.length === 0 ? null : <p class="hint">{row.notes.join(", ")}</p>}
            </div>
            <button
              type="button"
              class="button button-secondary"
              disabled={busy || env.disabled}
              onClick={() => void remove([row.id])}
            >
              {uiText(language, "answerDelete")}
            </button>
          </li>
        ))}
      </ul>
      <button
        type="button"
        class="button button-secondary"
        disabled={busy || env.disabled}
        onClick={() => void remove("all")}
      >
        {uiText(language, "answersDeleteAll")}
      </button>
      <div role="alert">{problem ? <p class="error">{uiText(language, "runErrorUnexpected")}</p> : null}</div>
    </section>
  );
}
