import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { quantityAnswer, type DecisionRequest } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { budgetAfterEur, formatUnits, parseUnits, removedValueEur } from "../../run/units";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** The quantity question of a sale that needs a little more than the earlier purchases left. */
export function QuantityQuestion({ env, request }: Props) {
  const check = request.quantityCheck;
  // The input starts with the units left, but nothing is sent until the user presses the button.
  const [text, setText] = useState(check?.proposedUnits ?? "");

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const original = parseUnits(check.originalUnits);
  const entered = parseUnits(text);
  const removed = original !== undefined && entered !== undefined && entered <= original ? original - entered : undefined;
  const errorId = `quantity-error-${request.id}`;

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.name} (${check.isin})`}
      answer={quantityAnswer(text)}
    >
      {(messagesFor) => (
        <>
          <p>
            {uiTextWith(language, "quantityIntro", {
              date: check.saleDate,
              original: check.originalUnits,
              units: check.proposedUnits,
            })}
          </p>
          {check.transaction === undefined ? null : (
            <p class="hint">
              {uiTextWith(language, "transactionLine", {
                broker: brokerTitle(check.transaction.broker, language),
                id: check.transaction.transactionId,
              })}
            </p>
          )}
          <label class="field">
            <span>{uiText(language, "quantityInput")}</span>
            <input
              type="text"
              inputMode="decimal"
              value={text}
              aria-describedby={errorId}
              onInput={(event) => setText(event.currentTarget.value)}
            />
          </label>
          {removed === undefined ? null : (
            <p class="hint">
              {uiTextWith(language, "quantityImpact", {
                removed: formatUnits(removed),
                value: removedValueEur(check.unitValueEur, removed),
                used: budgetAfterEur(check.budgetUsedEur, check.unitValueEur, removed),
              })}
            </p>
          )}
          <div id={errorId}>{messagesFor(["correctedUnits"])}</div>
        </>
      )}
    </QuestionFrame>
  );
}
