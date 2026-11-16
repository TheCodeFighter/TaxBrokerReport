import { renderDiagnostic, type MessageCatalogue } from "../i18n/messages";
import { uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import { MAX_FILE_BYTES, MAX_FILES } from "./limits";
import type { SubmitOutcome } from "./submit";
import { taxpayerFieldError, type TaxpayerForm } from "./taxpayer";

/**
 * The text for a refused run (a 400 code of the server). Codes that the form cannot cause by
 * itself (a part name, a path) all have the same plain text.
 */
const codeTexts = new Map<string, UiTextKey>([
  ["no_broker_file", "runErrorNoBrokerFile"],
  ["too_many_files", "runErrorTooManyFiles"],
  ["unsupported_broker", "runErrorBroker"],
]);

export function refusedText(code: string): UiTextKey {
  return codeTexts.get(code) ?? "runErrorGeneral";
}

/** The text shown for a run that did not give a result; for a taxpayer error also the input to mark. */
export interface ShownError {
  text: string;
  field?: { field: keyof TaxpayerForm | undefined; text: string };
}

const megabytes = MAX_FILE_BYTES / (1024 * 1024);

/** The text of a refused or failed run, in the language of the page. */
export function runErrorText(
  outcome: Exclude<SubmitOutcome, { kind: "result" }>,
  language: Language,
  catalogue: MessageCatalogue | undefined,
): ShownError {
  const text = (key: UiTextKey, values: Record<string, string | number> = {}) =>
    uiTextWith(language, key, values);

  switch (outcome.kind) {
    case "tooLarge":
      return {
        text:
          outcome.fileName === undefined
            ? text("runErrorTooLarge", { n: megabytes })
            : text("runErrorTooLargeNamed", { name: outcome.fileName, n: megabytes }),
      };
    case "inputFault": {
      const { fault } = outcome;
      const message =
        catalogue === undefined
          ? text("runErrorGeneral")
          : renderDiagnostic(catalogue, fault.messageKey, fault.messageParams, language);
      const where = [
        fault.source?.file,
        fault.source?.row === undefined ? undefined : text("runErrorFaultRow", { row: fault.source.row }),
      ]
        .filter((part) => part !== undefined)
        .join(", ");
      const head = where === "" ? text("runErrorFaultNoSource") : text("runErrorFault", { name: where });

      return { text: `${head} ${message}` };
    }
    case "refused": {
      if (outcome.code === "bad_taxpayer") {
        const error = taxpayerFieldError(outcome.field);

        return { text: text(error.text), field: { field: error.field, text: text(error.text) } };
      }

      return { text: text(refusedText(outcome.code), { n: MAX_FILES }) };
    }
    case "unexpected":
      return { text: text("runErrorUnexpected") };
  }
}
