import { useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import { loadReportXml } from "../api/report_xml";
import { saveFile } from "../api/saved_file";
import type { Language, UiTextKey } from "../i18n/ui_texts";
import { uiText, uiTextWith } from "../i18n/ui_texts";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
  /** The report key of the result (`dividends`). */
  reportKey: string;
  /** The form name shown on the button (`Doh-Div`). */
  form: string;
  /** The revision of the shown result; the server refuses a file of any other run. */
  revision: number;
  /** True while the shown result is out of date. */
  disabled: boolean;
  /** True for the developer export: a red button that says UNSAFE. */
  unsafe?: boolean;
}

/** Saves the XML file of one generated form, with the note that it is not filed yet. */
export function ReportDownloadButton({ language, apiFetch, reportKey, form, revision, disabled, unsafe = false }: Props) {
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState<UiTextKey | undefined>(undefined);

  const save = async () => {
    setBusy(true);
    setProblem(undefined);

    try {
      const answer = await loadReportXml(apiFetch, reportKey, form, revision);

      if ("file" in answer) {
        saveFile(answer.file);
      } else {
        setProblem(answer.problem === "stale" ? "reportSaveStale" : "runErrorUnexpected");
      }
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setProblem("runErrorUnexpected");
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="report-download">
      {unsafe ? <p class="danger-notice" role="alert">{uiText(language, "unsafeNotice")}</p> : null}
      <button
        type="button"
        class={unsafe ? "button button-danger" : "button"}
        disabled={busy || disabled}
        onClick={() => void save()}
      >
        {uiTextWith(language, unsafe ? "reportSaveUnsafe" : "reportSave", { form })}
      </button>
      <p class="hint">{uiText(language, "reportSaveHint")}</p>
      <div role="alert">{problem === undefined ? null : <p class="error">{uiText(language, problem)}</p>}</div>
    </div>
  );
}
