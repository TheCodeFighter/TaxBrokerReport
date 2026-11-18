import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { januaryHint, type DecisionRequest, type LossChoice, type LossSale } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

interface SaleProps {
  env: QuestionEnv;
  sale: LossSale;
}

/** The facts of one open sale: its units, its 30-day window, the known purchases in it. */
function SaleFacts({ env, sale }: SaleProps) {
  const { language } = env;

  return (
    <div class="question-sale">
      <p class="question-sale-title">{uiTextWith(language, "lossSaleTitle", { date: sale.saleDate })}</p>
      <ul class="plain-list">
        {sale.transaction === undefined ? null : (
          <li>
            {uiTextWith(language, "transactionLine", {
              broker: brokerTitle(sale.transaction.broker, language),
              id: sale.transaction.transactionId,
            })}
          </li>
        )}
        <li>{uiTextWith(language, "lossSaleUnits", { units: sale.saleUnits })}</li>
        <li>{uiTextWith(language, "lossLossUnits", { units: sale.lossUnits })}</li>
        <li>{uiTextWith(language, "lossWindow", { start: sale.windowStart, end: sale.windowEnd })}</li>
        <li>
          {sale.knownReplacements.length === 0
            ? uiText(language, "lossNoReplacements")
            : uiText(language, "lossReplacementsTitle")}
          {sale.knownReplacements.length === 0 ? null : (
            <ul class="plain-list">
              {sale.knownReplacements.map((replacement, index) => (
                <li key={index}>
                  {uiTextWith(language, "lossReplacement", { date: replacement.date, units: replacement.units })}
                </li>
              ))}
            </ul>
          )}
        </li>
        <li>{uiTextWith(language, "lossIneligible", { units: sale.knownIneligibleUnits })}</li>
        <li>{uiTextWith(language, "lossOpenUnits", { units: sale.openUnits })}</li>
      </ul>
      {januaryHint(sale) ? <p class="hint">{uiText(language, "lossJanuary")}</p> : null}
    </div>
  );
}

/** The 30-day loss question of one investment. Neither choice is preselected. */
export function LossQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<LossChoice | undefined>(undefined);
  const check = request.lossCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const radioName = `loss-${request.id}`;
  const choices: { value: LossChoice; label: "lossChoiceNo" | "lossChoiceYes" }[] = [
    { value: "no_other_activity", label: "lossChoiceNo" },
    { value: "other_activity", label: "lossChoiceYes" },
  ];

  return (
    <QuestionFrame env={env} request={request} title={`${check.name} (${check.isin})`} answer={choice}>
      {() => (
        <>
          <p>{uiText(language, "lossIntro")}</p>
          {check.sales.map((sale, index) => (
            <SaleFacts key={index} env={env} sale={sale} />
          ))}
          <fieldset class="field">
            <legend>{uiText(language, "lossQuestion")}</legend>
            {choices.map((entry) => (
              <label key={entry.value} class="radio">
                <input
                  type="radio"
                  name={radioName}
                  value={entry.value}
                  checked={choice === entry.value}
                  onChange={() => setChoice(entry.value)}
                />
                {uiText(language, entry.label)}
              </label>
            ))}
          </fieldset>
          {choice === "other_activity" ? (
            <p class="notice notice-warn">
              <span aria-hidden="true">⚠ </span>
              {uiText(language, "lossOtherWarning")}
            </p>
          ) : null}
        </>
      )}
    </QuestionFrame>
  );
}
