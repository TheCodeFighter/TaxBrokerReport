import { uiText, type Language } from "../i18n/ui_texts";
import { benefitView, feeView, type SummaryView } from "../run/income_view";
import type { RunResult } from "../run/submit";
import { ResultRegion } from "./result_region";

interface Props {
  language: Language;
  result: RunResult;
}

function SummaryList({ language, title, view }: { language: Language; title: string; view: SummaryView }) {
  return (
    <>
      <h4>{title}</h4>
      <ul class="plain-list block-list">
        {view.rows.map((row, index) => (
          <li key={index}>
            <strong>{row.title}</strong>
            <ul class="plain-list">
              {row.lines.map((line) => (
                <li key={line}>{line}</li>
              ))}
              <li>{row.total}</li>
              {row.converted === undefined ? null : <li class="hint">{row.converted}</li>}
            </ul>
          </li>
        ))}
        <li class="summary-total">
          <strong>{`${uiText(language, "summaryOverall")}: `}</strong>
          {view.total}
        </li>
      </ul>
    </>
  );
}

/** Fees and benefits of the year by broker and currency. Information only: no form holds them. */
export function FeeSummary({ language, result }: Props) {
  const fees = feeView(result.feeSummary, language);
  const benefits = benefitView(result.benefitSummary, language);

  if (fees === undefined && benefits === undefined) {
    return null;
  }

  return (
    <ResultRegion id="fees-title" title={uiText(language, "feesTitle")} hint={uiText(language, "feesHint")}>
      {fees === undefined ? null : <SummaryList language={language} title={uiText(language, "feesFees")} view={fees} />}
      {benefits === undefined ? null : (
        <SummaryList language={language} title={uiText(language, "feesBenefits")} view={benefits} />
      )}
    </ResultRegion>
  );
}
