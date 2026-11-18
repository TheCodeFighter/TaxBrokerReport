import { uiText, type Language } from "../i18n/ui_texts";
import { incomeGroups, incomeSummaryLine } from "../run/income_view";
import type { IncomeSummary } from "../run/submit";

interface Props {
  language: Language;
  summary: IncomeSummary | undefined;
}

/**
 * The income rows of one form, closed by default so a year with many payments does not push the
 * download away. The summary line shows the count, the gross income and the foreign tax.
 */
export function IncomeRows({ language, summary }: Props) {
  if (summary === undefined || summary.rows.length === 0) {
    return null;
  }

  return (
    <details class="income-rows">
      <summary>{incomeSummaryLine(summary, language)}</summary>
      {incomeGroups(summary, language).map((group) => (
        <div key={group.title} class="table-scroll">
          <table class="income-table">
            <caption>{group.title}</caption>
            <thead>
              <tr>
                <th scope="col">{uiText(language, "incomeColDate")}</th>
                <th scope="col">{uiText(language, "incomeColSubject")}</th>
                <th scope="col">{uiText(language, "incomeColPayer")}</th>
                <th scope="col">{uiText(language, "incomeColGross")}</th>
                <th scope="col">{uiText(language, "incomeColTax")}</th>
                <th scope="col">{uiText(language, "incomeColTreaty")}</th>
                <th scope="col">{uiText(language, "incomeColEvidence")}</th>
              </tr>
            </thead>
            <tbody>
              {group.rows.map((row, index) => (
                <tr key={index}>
                  <td>{row.date}</td>
                  <td>{row.subject}</td>
                  <td>
                    {row.payer}
                    {row.payerSource === undefined ? null : <span class="hint"> {row.payerSource}</span>}
                  </td>
                  <td>{row.gross}</td>
                  <td>{row.tax}</td>
                  <td>{row.treaty}</td>
                  <td>{row.evidence ? uiText(language, "incomeEvidenceYes") : ""}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
    </details>
  );
}
