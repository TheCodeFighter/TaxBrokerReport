import { uiTextWith } from "../../i18n/ui_texts";
import { questionKind, type DecisionRequest } from "../../run/questions";
import { BondQuestion } from "./bond_question";
import { ClassificationQuestion } from "./classification_question";
import { DepositQuestion } from "./deposit_question";
import { DeveloperConfirmation } from "./developer_confirmation";
import { FailureQuestion } from "./failure_question";
import { IncomeQuestion } from "./income_question";
import { LossQuestion } from "./loss_question";
import { PayerQuestion } from "./payer_question";
import { QuantityQuestion } from "./quantity_question";
import type { QuestionEnv } from "./question_frame";
import { RatioQuestion } from "./ratio_question";
import { TreatyQuestion } from "./treaty_question";

interface Props {
  env: QuestionEnv;
  requests: DecisionRequest[];
}

/** One screen for each open question of a form; a type without a screen shows nothing here. */
function QuestionView({ env, request }: { env: QuestionEnv; request: DecisionRequest }) {
  const kind = questionKind(request);

  switch (kind) {
    case "loss":
      return <LossQuestion env={env} request={request} />;
    case "ratio":
      return <RatioQuestion env={env} request={request} />;
    case "quantity":
      return <QuantityQuestion env={env} request={request} />;
    case "classification":
      return <ClassificationQuestion env={env} request={request} />;
    case "income":
      return <IncomeQuestion env={env} request={request} />;
    case "payer":
      return <PayerQuestion env={env} request={request} />;
    case "bond":
      return <BondQuestion env={env} request={request} />;
    case "treaty":
      return <TreatyQuestion env={env} request={request} />;
    case "deposit":
      return <DepositQuestion env={env} request={request} />;
    case "failure":
      return <FailureQuestion env={env} request={request} />;
    case "developer":
      return <DeveloperConfirmation env={env} request={request} />;
    case undefined:
      return null;
  }
}

/**
 * The open questions of one form, above its messages. Each question starts fresh with every new
 * result: the request IDs are numbered again for each result, so an ID can name another question.
 */
export function QuestionList({ env, requests }: Props) {
  if (requests.length === 0) {
    return null;
  }

  return (
    <div class="questions">
      <h4>{uiTextWith(env.language, "questionsTitle", { count: requests.length })}</h4>
      {requests.map((request) => (
        <QuestionView key={`${env.revision}-${request.id}`} env={env} request={request} />
      ))}
    </div>
  );
}
