import { uiText } from "../../i18n/ui_texts";
import type { Language } from "../../i18n/ui_texts";
import { correctionLines } from "../../run/decisions_view";
import type { Correction, Reconciliation } from "../../run/questions";

interface Props {
  language: Language;
  corrections: Correction[];
  reconciliation: Reconciliation[];
}

/** The sale quantities that were corrected, and the total impact per investment, in the Doh-KDVP card. */
export function CorrectionsBlock({ language, corrections, reconciliation }: Props) {
  if (corrections.length === 0) {
    return null;
  }

  const lines = correctionLines(corrections, reconciliation, language);

  return (
    <>
      <h4>{uiText(language, "correctionsTitle")}</h4>
      <ul class="plain-list">
        {lines.corrections.map((line, index) => (
          <li key={index}>{line}</li>
        ))}
        {lines.reconciliation.map((line, index) => (
          <li key={`total-${index}`}>{line}</li>
        ))}
      </ul>
    </>
  );
}
