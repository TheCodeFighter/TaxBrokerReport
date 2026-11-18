import { uiText, type Language } from "../i18n/ui_texts";
import { capitalView } from "../run/capital_view";
import type { CapitalSummary } from "../run/submit";

interface Props {
  language: Language;
  summary: CapitalSummary | undefined;
}

/**
 * The investments with a sale in the year and what the Doh-KDVP form does with each one, closed by
 * default like the income rows. Information only: the XML file is the truth.
 */
export function CapitalRows({ language, summary }: Props) {
  const view = capitalView(summary, language);

  if (view === undefined) {
    return null;
  }

  return (
    <details class="income-rows">
      <summary>{view.summaryLine}</summary>
      {view.rows.length === 0 ? null : (
        <div class="table-scroll">
          <table class="income-table">
            <caption>{uiText(language, "capitalCaption")}</caption>
            <thead>
              <tr>
                <th scope="col">{uiText(language, "capitalColInvestment")}</th>
                <th scope="col">{uiText(language, "capitalColState")}</th>
                <th scope="col">{uiText(language, "capitalColSales")}</th>
                <th scope="col">{uiText(language, "capitalColUnits")}</th>
                <th scope="col">{uiText(language, "capitalColBlocked")}</th>
                <th scope="col">{uiText(language, "capitalColDates")}</th>
              </tr>
            </thead>
            <tbody>
              {view.rows.map((row, index) => (
                <tr key={index}>
                  <td>{row.investment}</td>
                  <td>{row.state}</td>
                  <td>{row.sales}</td>
                  <td>{row.units}</td>
                  <td>{row.blockedUnits}</td>
                  <td>{row.dates}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {view.longHeld.length === 0 ? null : (
        <>
          <h4>{uiText(language, "capitalLongHeldTitle")}</h4>
          <ul class="plain-list">
            {view.longHeld.map((line, index) => (
              <li key={index}>{line}</li>
            ))}
          </ul>
        </>
      )}
    </details>
  );
}
