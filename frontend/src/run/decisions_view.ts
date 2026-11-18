import { uiText, uiTextWith, type Language } from "../i18n/ui_texts";
import { questionKind, type Correction, type Decision, type Reconciliation } from "./questions";
import { fractionToEur4 } from "./units";

/** One recorded answer as the "Your answers" list shows it. */
export interface DecisionRow {
  id: string;
  /** The kind of question, for example "30-day rule". */
  title: string;
  /** The investment the answer belongs to, `Name (ISIN)`; not set for a question without one. */
  investment?: string;
  answer: string;
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

function investmentOf(decision: Decision): string | undefined {
  const check = decision.lossCheck ?? decision.ratioCheck ?? decision.quantityCheck;

  return check === undefined ? undefined : `${check.name} (${check.isin})`;
}

/** The recorded answers in result order, each with its kind, investment, answer text and notes. */
export function decisionRows(decisions: Decision[], language: Language): DecisionRow[] {
  return decisions.map((decision) => {
    const kind = questionKind(decision.type);
    const investment = investmentOf(decision);
    const notes = [
      decision.fromEarlierRun ? uiText(language, "answerFromEarlier") : undefined,
      decision.used ? undefined : uiText(language, "answerNotUsed"),
    ].filter((note) => note !== undefined);

    const title =
      kind === "loss"
        ? uiText(language, "decisionLoss")
        : kind === "ratio"
          ? uiText(language, "decisionRatio")
          : kind === "quantity"
            ? uiText(language, "decisionQuantity")
            : uiText(language, "decisionOther");
    const answer =
      kind === "loss"
        ? lossAnswerText(decision.answer, language)
        : kind === "ratio"
          ? ratioAnswerText(decision.answer, language)
          : kind === "quantity"
            ? quantityAnswerText(decision.answer, language)
            : uiText(language, "answerRecorded");

    return { id: decision.id, title, ...(investment === undefined ? {} : { investment }), answer, notes };
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
