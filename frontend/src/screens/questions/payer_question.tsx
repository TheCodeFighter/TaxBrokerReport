import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { payerAnswer, SKIP_ANSWER } from "../../run/income_questions";
import type { DecisionRequest } from "../../run/questions";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** Reference rows disagree on the payer of an ISIN: pick one row or skip (the facts stay empty). */
export function PayerQuestion({ env, request }: Props) {
  // The position of the picked row, `"skip"`, or nothing yet.
  const [picked, setPicked] = useState<number | "skip" | undefined>(undefined);
  const check = request.payerCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const radioName = `payer-${request.id}`;
  const answer = picked === "skip" ? SKIP_ANSWER : payerAnswer(picked);

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.name} (${check.isin})`}
      answer={answer}
      allowCancel={false}
    >
      {() => (
        <>
          <p>{uiText(language, "payerIntro")}</p>
          <fieldset class="field">
            <legend>{uiText(language, "payerQuestion")}</legend>
            {check.options.map((option, index) => (
              <label key={index} class="radio payer-option">
                <input
                  type="radio"
                  name={radioName}
                  value={String(index)}
                  checked={picked === index}
                  onChange={() => setPicked(index)}
                />
                <span>
                  <strong>{`${option.name} (${option.country})`}</strong>
                  <span class="hint">{option.address}</span>
                  {option.taxNumber === undefined ? null : (
                    <span class="hint">{uiTextWith(language, "payerTaxNumber", { number: option.taxNumber })}</span>
                  )}
                  <span class="hint">
                    {uiTextWith(language, "payerSource", {
                      file: option.source.file,
                      row: option.source.row,
                      revision: option.source.sourceRevision,
                    })}
                  </span>
                </span>
              </label>
            ))}
            <label class="radio">
              <input
                type="radio"
                name={radioName}
                value="skip"
                checked={picked === "skip"}
                onChange={() => setPicked("skip")}
              />
              {uiText(language, "payerSkip")}
            </label>
          </fieldset>
        </>
      )}
    </QuestionFrame>
  );
}
