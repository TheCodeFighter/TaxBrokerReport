import { useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import { saveFile } from "../api/saved_file";
import { loadSupportReport } from "../api/support_report";
import type { Language, UiTextKey } from "../i18n/ui_texts";
import { uiText } from "../i18n/ui_texts";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
}

/** Saves the support report of the run: a file for a problem report, with no private data. */
export function SupportReportButton({ language, apiFetch }: Props) {
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState<UiTextKey | undefined>(undefined);

  const save = async () => {
    setBusy(true);
    setProblem(undefined);

    try {
      const answer = await loadSupportReport(apiFetch);

      if ("file" in answer) {
        saveFile(answer.file);
      } else {
        setProblem(answer.problem === "no_run" ? "supportNoRun" : "runErrorUnexpected");
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
    <div class="support-report">
      <button type="button" class="button" disabled={busy} onClick={() => void save()}>
        {uiText(language, "supportReportSave")}
      </button>
      <p class="hint">{uiText(language, "supportReportHint")}</p>
      <div role="alert">{problem === undefined ? null : <p class="error">{uiText(language, problem)}</p>}</div>
    </div>
  );
}
