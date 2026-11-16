import { useEffect, useRef } from "preact/hooks";

import type { Language } from "../i18n/ui_texts";
import { uiText, uiTextWith } from "../i18n/ui_texts";
import {
  countDiagnostics,
  needsAnswerHint,
  overallStatusText,
  reportRows,
  type ShownResult,
} from "../run/result_view";

interface Props {
  language: Language;
  shown: ShownResult;
}

/** The result of the last run: its status, one line per form, the counts and the file list. */
export function RunResultView({ language, shown }: Props) {
  const { result } = shown;
  const counts = countDiagnostics(result);
  const titleRef = useRef<HTMLHeadingElement>(null);

  // A new result takes the focus, which also scrolls it into view and makes a screen reader read it.
  useEffect(() => {
    titleRef.current?.focus();
  }, [result]);

  return (
    <section class="run-section run-result" aria-labelledby="result-title">
      <h2 id="result-title" ref={titleRef} tabIndex={-1}>{uiText(language, "resultTitle")}</h2>
      {shown.outOfDate ? <p class="notice">{uiText(language, "resultOutOfDate")}</p> : null}
      <div class={shown.outOfDate ? "result result-old" : "result"}>
        <p class="result-status">{uiText(language, overallStatusText(result.status))}</p>
        <ul class="plain-list">
          {reportRows(result).map((row) => (
            <li key={row.form}>
              <strong>{row.form}</strong>: {uiText(language, row.statusText)}
            </li>
          ))}
        </ul>
        {needsAnswerHint(result) ? <p class="notice">{uiText(language, "answersLater")}</p> : null}
        <p>
          {uiTextWith(language, "resultCounts", { errors: counts.errors, warnings: counts.warnings })}
        </p>
        <h3>{uiText(language, "resultFiles")}</h3>
        <ol class="plain-list">
          {shown.fileLabels.map((label, index) => (
            <li key={index}>{label}</li>
          ))}
        </ol>
      </div>
    </section>
  );
}
