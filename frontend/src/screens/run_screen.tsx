import { useMemo, useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import type { MessageCatalogue } from "../i18n/messages";
import { uiText, uiTextWith, type Language } from "../i18n/ui_texts";
import { emptyInputs, type RunInputs } from "../run/inputs";
import { inputLabels } from "../run/file_labels";
import { markOutOfDate, type ShownResult } from "../run/result_view";
import { runErrorText, type ShownError } from "../run/run_errors";
import { submitRun } from "../run/submit";
import { RunForm } from "./run_form";
import { RunResultView } from "./run_result";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
  catalogue: MessageCatalogue | undefined;
}

/** The input screen of a run and, after a run, its result. One button sends the run; no polling. */
export function RunScreen({ language, apiFetch, catalogue }: Props) {
  const today = useMemo(() => new Date(), []);
  const [inputs, setInputs] = useState<RunInputs>(() => emptyInputs(today));
  const [sending, setSending] = useState(false);
  const [error, setError] = useState<ShownError | undefined>();
  const [shown, setShown] = useState<ShownResult | undefined>();

  // Any change of the inputs marks the shown result as out of date and clears the last error.
  const change = (changed: RunInputs) => {
    setInputs(changed);
    setError(undefined);
    setShown(markOutOfDate);
  };

  const send = async () => {
    const sent = inputs;
    const suffix = (number: number) => uiTextWith(language, "fileSuffix", { n: number });

    setSending(true);
    setError(undefined);

    try {
      const outcome = await submitRun(apiFetch, sent);

      if (outcome.kind === "result") {
        setShown({ result: outcome.result, fileLabels: inputLabels(sent, suffix).broker, outOfDate: false });
      } else {
        setError(runErrorText(outcome, language, catalogue));
      }
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setError({ text: uiText(language, "runErrorUnexpected") });
      }
    } finally {
      setSending(false);
    }
  };

  return (
    <>
      <RunForm
        language={language}
        today={today}
        inputs={inputs}
        onChange={change}
        fieldError={error?.field}
        sending={sending}
        outOfDate={shown?.outOfDate === true}
        onSubmit={() => void send()}
      />
      <div role="alert">{error === undefined ? null : <p class="error">{error.text}</p>}</div>
      {shown === undefined ? null : <RunResultView language={language} shown={shown} />}
    </>
  );
}
