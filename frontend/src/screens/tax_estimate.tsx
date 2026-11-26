import { uiText, type Language } from "../i18n/ui_texts";
import { taxEstimateView } from "../run/income_view";
import type { RunResult } from "../run/submit";
import { ResultRegion } from "./result_region";

interface Props {
  language: Language;
  result: RunResult;
}

/** The approximate tax to pay, one line for each form. Information only: no form holds it. */
export function TaxEstimate({ language, result }: Props) {
  const view = taxEstimateView(result.taxEstimate, result.depositInterest?.addOtherBanks === true, language);

  if (view === undefined) {
    return null;
  }

  return (
    <ResultRegion id="tax-estimate-title" title={uiText(language, "taxTitle")} hint={uiText(language, "taxHint")}>
      {view.unsupported ? (
        <p>{uiText(language, "taxUnsupported")}</p>
      ) : (
        <ul class="plain-list block-list">
          {view.rows.map((row) => (
            <li key={row.form}>
              <strong>{`${row.form}: `}</strong>
              {row.text}
              {row.note === undefined ? null : <span class="hint">{` ${row.note}`}</span>}
            </li>
          ))}
          <li class="summary-total">{view.total}</li>
        </ul>
      )}
    </ResultRegion>
  );
}
