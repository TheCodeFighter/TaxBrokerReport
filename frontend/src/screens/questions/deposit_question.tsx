import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import type { DecisionRequest } from "../../run/questions";
import { ChoiceGroup } from "./choice_group";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** The yes/no question whether the deposit interest of all banks is above the allowance. No cancel. */
export function DepositQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<string | undefined>(undefined);
  const check = request.depositCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={uiTextWith(language, "depositTitle", { year: check.year })}
      answer={choice}
      allowCancel={false}
    >
      {() => (
        <>
          <p>
            {uiTextWith(language, "depositIntro", {
              imported: check.importedInterestEur,
              limit: check.limitEur,
              year: check.year,
            })}
          </p>
          <ChoiceGroup
            language={language}
            name={`deposit-${request.id}`}
            legend={uiText(language, "depositQuestion")}
            options={["above_limit", "not_above_limit"]}
            value={choice}
            onChange={setChoice}
          />
        </>
      )}
    </QuestionFrame>
  );
}
