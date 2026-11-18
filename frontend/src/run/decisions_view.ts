import { uiText, uiTextWith, type Language } from "../i18n/ui_texts";
import { failureForm } from "./failure_question";
import { choiceText } from "./income_questions";
import { questionKind, type Correction, type Decision, type QuestionKind, type Reconciliation } from "./questions";
import { brokerTitle } from "./result_view";
import { fractionToEur4 } from "./units";

/** One recorded answer as the "Your answers" list shows it. */
export interface DecisionRow {
  id: string;
  /** The kind of question, for example "30-day rule". */
  title: string;
  /** The investment the answer belongs to, `Name (ISIN)`; not set for a question without one. */
  investment?: string;
  answer: string;
  /** The investments and brokers a failure answer lists, one line each; empty for other answers. */
  details: string[];
  /** Short notes: from an earlier run, not used in this result. */
  notes: string[];
}

function lossAnswerText(answer: Decision["answer"], language: Language): string {
  if (answer === "no_other_activity") {
    return uiText(language, "lossChoiceNo");
  }

  if (answer === "other_activity") {
    return uiText(language, "lossChoiceYes");
  }

  return answer === "cancel" ? uiText(language, "answerCancelled") : uiText(language, "answerRecorded");
}

function ratioAnswerText(answer: Decision["answer"], language: Language): string {
  if (answer === "cancel") {
    return uiText(language, "answerCancelled");
  }

  if (answer === "separate_events") {
    return uiText(language, "answerSeparateEvents");
  }

  if (typeof answer === "object") {
    return uiTextWith(language, "answerRatio", {
      new: String(answer.newShares),
      old: String(answer.oldShares),
      date: String(answer.effectiveDate),
      units: String(answer.confirmedUnits),
    });
  }

  return uiText(language, "answerRecorded");
}

function quantityAnswerText(answer: Decision["answer"], language: Language): string {
  if (answer === "cancel") {
    return uiText(language, "answerCancelled");
  }

  if (typeof answer === "object") {
    return uiTextWith(language, "answerQuantity", { units: String(answer.correctedUnits) });
  }

  return uiText(language, "answerRecorded");
}

/** A word answer as its choice text; `cancel` and an answer the page does not know have their own. */
function wordAnswerText(answer: Decision["answer"], language: Language): string {
  if (answer === "cancel") {
    return uiText(language, "answerCancelled");
  }

  const key = typeof answer === "string" ? choiceText(answer) : undefined;

  return key === undefined ? uiText(language, "answerRecorded") : uiText(language, key);
}

function incomeAnswerText(answer: Decision["answer"], language: Language): string {
  if (typeof answer === "object") {
    const subtype = typeof answer.subtype === "string" ? choiceText(answer.subtype) : undefined;
    const choice = typeof answer.choice === "string" ? choiceText(answer.choice) : undefined;

    if (choice !== undefined && subtype !== undefined) {
      return `${uiText(language, choice)}: ${uiText(language, subtype)}`;
    }
  }

  return wordAnswerText(answer, language);
}

function payerAnswerText(decision: Decision, language: Language): string {
  const { answer } = decision;

  if (answer === "skip") {
    return uiText(language, "answerPayerSkipped");
  }

  if (typeof answer === "object" && typeof answer.option === "number") {
    const option = decision.payerCheck?.options[answer.option];

    if (option !== undefined) {
      return uiTextWith(language, "answerPayer", {
        name: option.name,
        country: option.country,
        file: option.source.file,
        row: option.source.row,
      });
    }
  }

  return wordAnswerText(answer, language);
}

function treatyAnswerText(answer: Decision["answer"], language: Language): string {
  if (answer === "no_claim") {
    return uiText(language, "answerNoClaim");
  }

  if (typeof answer === "object" && typeof answer.statement === "string") {
    return uiTextWith(language, "answerTreaty", { statement: answer.statement });
  }

  return wordAnswerText(answer, language);
}

function failureAnswerText(decision: Decision, language: Language): string {
  if (decision.answer === "exclude_failed_instruments") {
    const count = (decision.investments?.length ?? 0) + (decision.brokers?.length ?? 0);

    return uiTextWith(language, "answerFailExclude", { count });
  }

  if (decision.answer === "fail_report") {
    return uiText(language, "answerFailReport");
  }

  return wordAnswerText(decision.answer, language);
}

/** The lines of the investments and brokers a failure answer lists. */
function failureDetails(decision: Decision, language: Language): string[] {
  return [
    ...(decision.investments ?? []).map((investment) => `${investment.name} (${investment.isin})`),
    ...(decision.brokers ?? []).map((broker) => brokerTitle(broker.broker, language)),
  ];
}

/** The investment (or the country, or the year) an answer belongs to; not set when it has none. */
function investmentOf(decision: Decision, kind: QuestionKind | undefined): string | undefined {
  if (kind === "income") {
    const check = decision.incomeCheck;

    return check === undefined ? undefined : check.isin === undefined ? check.name : `${check.name} (${check.isin})`;
  }

  if (kind === "treaty") {
    return decision.treatyCheck === undefined ? undefined : `${decision.treatyCheck.country}, ${decision.treatyCheck.year}`;
  }

  if (kind === "deposit") {
    return decision.depositCheck === undefined ? undefined : String(decision.depositCheck.year);
  }

  const check =
    decision.lossCheck ??
    decision.ratioCheck ??
    decision.quantityCheck ??
    decision.classificationCheck ??
    decision.payerCheck ??
    decision.bondCheck;

  return check === undefined ? undefined : `${check.name} (${check.isin})`;
}

/** The title and the answer text of a recorded answer, by the kind of its question. */
function describeDecision(
  decision: Decision,
  kind: QuestionKind | undefined,
  language: Language,
): { title: string; answer: string } {
  switch (kind) {
    case "loss":
      return { title: uiText(language, "decisionLoss"), answer: lossAnswerText(decision.answer, language) };
    case "ratio":
      return { title: uiText(language, "decisionRatio"), answer: ratioAnswerText(decision.answer, language) };
    case "quantity":
      return { title: uiText(language, "decisionQuantity"), answer: quantityAnswerText(decision.answer, language) };
    case "classification":
      return { title: uiText(language, "decisionClassification"), answer: wordAnswerText(decision.answer, language) };
    case "income":
      return { title: uiText(language, "decisionIncome"), answer: incomeAnswerText(decision.answer, language) };
    case "payer":
      return { title: uiText(language, "decisionPayer"), answer: payerAnswerText(decision, language) };
    case "bond":
      return { title: uiText(language, "decisionBond"), answer: wordAnswerText(decision.answer, language) };
    case "treaty":
      return { title: uiText(language, "decisionTreaty"), answer: treatyAnswerText(decision.answer, language) };
    case "deposit":
      return { title: uiText(language, "decisionDeposit"), answer: wordAnswerText(decision.answer, language) };
    case "failure":
      return {
        title: uiTextWith(language, "decisionFailure", { form: failureForm(decision.type) ?? "" }),
        answer: failureAnswerText(decision, language),
      };
    case undefined:
      return { title: uiText(language, "decisionOther"), answer: uiText(language, "answerRecorded") };
  }
}

/** The recorded answers in result order, each with its kind, investment, answer text and notes. */
export function decisionRows(decisions: Decision[], language: Language): DecisionRow[] {
  return decisions.map((decision) => {
    const kind = questionKind(decision);
    const investment = investmentOf(decision, kind);
    const notes = [
      decision.fromEarlierRun ? uiText(language, "answerFromEarlier") : undefined,
      decision.used ? undefined : uiText(language, "answerNotUsed"),
    ].filter((note) => note !== undefined);

    return {
      id: decision.id,
      ...describeDecision(decision, kind, language),
      ...(investment === undefined ? {} : { investment }),
      details: kind === "failure" ? failureDetails(decision, language) : [],
      notes,
    };
  });
}

/** The corrected sale quantities and the cumulative impact per ISIN, as lines of text. */
export function correctionLines(
  corrections: Correction[],
  reconciliation: Reconciliation[],
  language: Language,
): { corrections: string[]; reconciliation: string[] } {
  return {
    corrections: corrections.map((correction) =>
      uiTextWith(language, "correctionRow", {
        isin: correction.isin,
        date: correction.saleDate,
        original: correction.originalUnits,
        corrected: correction.correctedUnits,
        kind: uiText(language, correction.kind === "rounding_evidence" ? "correctionRounding" : "correctionUser"),
        impact: fractionToEur4(correction.impactEur),
      }),
    ),
    reconciliation: reconciliation.map((impact) =>
      uiTextWith(language, "reconciliationRow", { isin: impact.isin, impact: fractionToEur4(impact.impactEur) }),
    ),
  };
}
