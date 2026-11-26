import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import type { DecisionRequest } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import {
  initialStockMergerForm,
  stockMergerAnswer,
  stockMergerProblems,
  type StockMergerForm,
} from "../../run/stock_merger_question";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/**
 * The question of a merger of two companies: shares of the old ISIN left and shares of the new
 * ISIN came in. The user says how it is taxed (a sale at the market value of the new shares, or a
 * deferral FURS decided) and gives the value (for a sale) and the date. Nothing is preselected.
 */
export function StockMergerQuestion({ env, request }: Props) {
  const check = request.stockMergerCheck;
  const [form, setForm] = useState<StockMergerForm | undefined>(undefined);

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const current = form ?? initialStockMergerForm(check);
  const problems = stockMergerProblems(check, current);
  const answer = stockMergerAnswer(check, current);
  const change = (changed: Partial<StockMergerForm>) => setForm({ ...current, ...changed });
  const radioName = `stock-merger-${request.id}`;

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
          <h6>{uiText(language, "stockMergerTitle")}</h6>
          <p>{uiText(language, "stockMergerIntro")}</p>
          <p>
            {uiTextWith(language, "stockMergerUnits", {
              old: check.oldUnits,
              new: check.newUnits,
              cash: check.cash,
              currency: check.currency,
            })}
          </p>
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
            {(["taxable", "deferred"] as const).map((value) => (
              <label key={value} class="radio">
                <input
                  type="radio"
                  name={radioName}
                  value={value}
                  checked={current.treatment === value}
                  onChange={() => change({ treatment: value })}
                />
                {uiText(language, value === "taxable" ? "stockMergerTaxable" : "stockMergerDeferred")}
              </label>
            ))}
          </fieldset>
          {current.treatment === undefined ? null : (
            <>
              {current.treatment !== "taxable" ? null : (
                <>
                  <label class="field">
                    <span>{uiTextWith(language, "stockMergerValue", { currency: check.currency })}</span>
                    <input
                      type="text"
                      inputMode="decimal"
                      value={current.value}
                      onInput={(event) => change({ value: event.currentTarget.value })}
                    />
                    {check.suggestedValue === undefined ? null : (
                      <span class="hint">{uiTextWith(language, "stockMergerValueRows", { value: check.suggestedValue })}</span>
                    )}
                  </label>
                  {!problems.value ? null : (
                    <p class="field-error" role="alert">
                      {uiText(language, "stockMergerValueNotValid")}
                    </p>
                  )}
                  {messagesFor(["marketValue"])}
                </>
              )}
              <label class="field">
                <span>{uiText(language, "stockMergerDate")}</span>
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
