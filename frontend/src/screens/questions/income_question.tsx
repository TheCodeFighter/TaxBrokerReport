import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { incomeAnswer, OTHER_INTEREST, shownClassLabels } from "../../run/income_questions";
import type { DecisionRequest } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { ChoiceGroup } from "./choice_group";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** An income payment of an unknown type: dividend, deposit interest, other interest or left out. */
export function IncomeQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<string | undefined>(undefined);
  const [subtype, setSubtype] = useState<string | undefined>(undefined);
  const check = request.incomeCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const labels = shownClassLabels(check.classLabels);
  const title = check.isin === undefined ? check.name : `${check.name} (${check.isin})`;

  return (
    <QuestionFrame env={env} request={request} title={title} answer={incomeAnswer(choice, subtype)} allowCancel>
      {() => (
        <>
          <p>
            {uiTextWith(language, "incomeIntro", {
              kind: uiText(language, check.eventKind === "dividend" ? "incomeEventDividend" : "incomeEventInterest"),
              date: check.receiptDate,
              amount: check.grossAmount,
              currency: check.currency,
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
          <p class="hint">
            {labels.length === 0
              ? uiText(language, "classLabelsNone")
              : uiTextWith(language, "classLabels", { labels: labels.join(", ") })}
          </p>
          <ChoiceGroup
            language={language}
            name={`income-${request.id}`}
            legend={uiText(language, "incomeQuestion")}
            options={request.options ?? []}
            value={choice}
            onChange={setChoice}
          />
          {choice !== OTHER_INTEREST ? null : (
            <ChoiceGroup
              language={language}
              name={`income-kind-${request.id}`}
              legend={uiText(language, "incomeSubtypeQuestion")}
              options={request.otherInterestSubtypes ?? []}
              value={subtype}
              onChange={setSubtype}
            />
          )}
        </>
      )}
    </QuestionFrame>
  );
}
