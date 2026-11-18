import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import type { DecisionRequest } from "../../run/questions";
import { ChoiceGroup } from "./choice_group";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** The kind of a bond that was sold or matured in the year: ordinary, or discount or zero coupon. */
export function BondQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<string | undefined>(undefined);
  const check = request.bondCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.name} (${check.isin})`}
      answer={choice}
      allowCancel
    >
      {() => (
        <>
          <p>{uiText(language, "bondIntro")}</p>
          <ul class="plain-list">
            <li>{uiTextWith(language, "bondSold", { units: check.soldUnits })}</li>
            <li>{uiTextWith(language, "bondMatured", { units: check.maturedUnits })}</li>
            {check.maturityDate === undefined ? null : (
              <li>{uiTextWith(language, "bondMaturityDate", { date: check.maturityDate })}</li>
            )}
          </ul>
          <ChoiceGroup
            language={language}
            name={`bond-${request.id}`}
            legend={uiText(language, "bondQuestion")}
            options={request.options ?? []}
            value={choice}
            onChange={setChoice}
          />
        </>
      )}
    </QuestionFrame>
  );
}
