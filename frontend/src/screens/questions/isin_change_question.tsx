import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import {
  initialIsinChangeForm,
  isinChangeAnswer,
  isinChangeProblems,
  type IsinChangeForm,
} from "../../run/isin_change_question";
import type { DecisionRequest } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/**
 * The question of an ISIN change: the units of the old ISIN left and units of the new ISIN came
 * in. The user says whether it is the same issuer, the same kind of share and no cash, and then
 * gives the ratio and the date; "no" makes it a merger of two companies. Nothing is preselected.
 */
export function IsinChangeQuestion({ env, request }: Props) {
  const check = request.isinChangeCheck;
  const [form, setForm] = useState<IsinChangeForm | undefined>(undefined);

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const current = form ?? initialIsinChangeForm(check);
  const problems = isinChangeProblems(check, current);
  const answer = isinChangeAnswer(check, current);
  const change = (changed: Partial<IsinChangeForm>) => setForm({ ...current, ...changed });
  const radioName = `isin-change-${request.id}`;

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.oldName} (${check.oldIsin}) → ${check.newName} (${check.newIsin})`}
      answer={answer}
      allowCancel
    >
      {(messagesFor) => (
        <>
          <h6>{uiText(language, "isinChangeTitle")}</h6>
          <p>{uiText(language, "isinChangeIntro")}</p>
          <h6>{uiText(language, "isinChangeSourcesTitle")}</h6>
          <ul class="plain-list">
            {check.sources.map((source, index) => {
              const broker = brokerTitle(source.broker, language);

              return (
                <li key={index}>
                  {source.transactionId === undefined
                    ? uiTextWith(language, "ratioSourceNoId", { broker, date: source.date })
                    : uiTextWith(language, "ratioSource", { broker, date: source.date, id: source.transactionId })}
                  {source.sourceValue === undefined ? null : (
                    <span class="hint"> {uiTextWith(language, "ratioSourceValue", { value: source.sourceValue })}</span>
                  )}
                </li>
              );
            })}
          </ul>
          <fieldset class="field">
            {(["yes", "no"] as const).map((value) => (
              <label key={value} class="radio">
                <input
                  type="radio"
                  name={radioName}
                  value={value}
                  checked={current.sameIssuer === value}
                  onChange={() => change({ sameIssuer: value })}
                />
                {uiText(language, value === "yes" ? "isinChangeYes" : "isinChangeNo")}
              </label>
            ))}
          </fieldset>
          {current.sameIssuer !== "yes" ? null : (
            <>
              <p class="hint">{uiText(language, "isinChangeRatioHint")}</p>
              {check.suggestedRatio === undefined ? null : (
                <p class="hint">
                  {uiTextWith(language, "isinChangeRatioRows", {
                    new: String(check.suggestedRatio.newShares),
                    old: String(check.suggestedRatio.oldShares),
                  })}
                </p>
              )}
              <label class="field">
                <span>{uiText(language, "ratioNew")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={current.newShares}
                  onInput={(event) => change({ newShares: event.currentTarget.value })}
                />
              </label>
              <label class="field">
                <span>{uiText(language, "ratioOld")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={current.oldShares}
                  onInput={(event) => change({ oldShares: event.currentTarget.value })}
                />
              </label>
              {messagesFor(["newShares", "oldShares"])}
              {!problems.numbers ? null : (
                <p class="field-error" role="alert">
                  {uiText(language, "isinChangeNumbersNotValid")}
                </p>
              )}
              <label class="field">
                <span>{uiText(language, "isinChangeDate")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={current.date}
                  onInput={(event) => change({ date: event.currentTarget.value })}
                />
                <span class="hint">
                  {uiTextWith(language, "isinChangeDateHint", { from: check.earliestDate, to: check.latestDate })}
                </span>
              </label>
              {!problems.date ? null : (
                <p class="field-error" role="alert">
                  {uiTextWith(language, "isinChangeDateNotValid", { from: check.earliestDate, to: check.latestDate })}
                </p>
              )}
              {messagesFor(["effectiveDate"])}
            </>
          )}
        </>
      )}
    </QuestionFrame>
  );
}
