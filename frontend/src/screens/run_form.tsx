import type { ComponentChildren } from "preact";

import type { Language } from "../i18n/ui_texts";
import { uiText, uiTextWith } from "../i18n/ui_texts";
import { addFiles, moveFile, removeFile, type FilingType, type RunInputs } from "../run/inputs";
import { brokerKinds } from "../run/input_kinds";
import { inputLabels } from "../run/file_labels";
import { taxpayerFields, type TaxpayerForm } from "../run/taxpayer";
import { hasWrongCheckDigit } from "../run/tax_number";
import {
  FIRST_BUILT_IN_RATE_YEAR,
  LAST_BUILT_IN_RATE_YEAR,
  needsRateHint,
  reportingYears,
} from "../run/years";

interface Props {
  language: Language;
  today: Date;
  inputs: RunInputs;
  onChange: (inputs: RunInputs) => void;
  /** The taxpayer field the server refused, marked until the next change. */
  fieldError?: { field: keyof TaxpayerForm | undefined; text: string };
  sending: boolean;
  outOfDate: boolean;
  /** The "Remember my details" box, shown under the taxpayer fields. */
  rememberBox?: ComponentChildren;
  /** True while the box asks something (the delete warning): the run waits for the answer. */
  submitBlocked?: boolean;
  onSubmit: () => void;
}

interface FileListProps {
  language: Language;
  title: string;
  /** A short line under the title naming the export to pick (the broker areas only). */
  format?: string;
  hint: string;
  /** The `accept` filter of the file dialog. */
  accept: string;
  files: File[];
  labels: string[];
  /** True for the broker files, where the order matters and is set with the arrows. */
  ordered: boolean;
  onChange: (files: File[]) => void;
}

/** One area of the form: a button that opens the file dialog and the list of the chosen files. */
function FileArea({ language, title, format, hint, accept, files, labels, ordered, onChange }: FileListProps) {
  const pick = (event: Event) => {
    const input = event.currentTarget as HTMLInputElement;

    onChange(addFiles(files, Array.from(input.files ?? [])));
    // The same file can be chosen again after it was removed.
    input.value = "";
  };

  return (
    <div class="file-area">
      <h3>{title}</h3>
      {format === undefined ? null : <p class="hint">{format}</p>}
      <p class="hint">{hint}</p>
      <label class="button button-secondary">
        {uiText(language, "chooseFiles")}
        <input class="visually-hidden" type="file" accept={accept} multiple onChange={pick} />
      </label>
      {files.length === 0 ? (
        <p class="hint">{uiText(language, "noFilesChosen")}</p>
      ) : (
        <ol class="file-list">
          {files.map((file, index) => (
            <li key={`${index}-${file.name}`}>
              <span class="file-name">{labels[index]}</span>
              {ordered ? (
                <>
                  <button
                    type="button"
                    class="icon-button"
                    disabled={index === 0}
                    aria-label={`${uiText(language, "moveUp")}: ${labels[index]}`}
                    onClick={() => onChange(moveFile(files, index, "up"))}
                  >
                    ↑
                  </button>
                  <button
                    type="button"
                    class="icon-button"
                    disabled={index === files.length - 1}
                    aria-label={`${uiText(language, "moveDown")}: ${labels[index]}`}
                    onClick={() => onChange(moveFile(files, index, "down"))}
                  >
                    ↓
                  </button>
                </>
              ) : null}
              <button
                type="button"
                class="icon-button"
                aria-label={`${uiText(language, "removeFile")}: ${labels[index]}`}
                onClick={() => onChange(removeFile(files, index))}
              >
                ✕
              </button>
            </li>
          ))}
        </ol>
      )}
    </div>
  );
}

/** The input screen: the files, the year, the filing type and the taxpayer, and the one button. */
export function RunForm({
  language,
  today,
  inputs,
  onChange,
  fieldError,
  sending,
  outOfDate,
  rememberBox,
  submitBlocked = false,
  onSubmit,
}: Props) {
  const suffix = (number: number) => uiTextWith(language, "fileSuffix", { n: number });
  const hasBrokerFile = brokerKinds.some((kind) => (inputs.brokerFiles[kind.broker] ?? []).length > 0);
  const labels = inputLabels(inputs, suffix);
  let labelStart = 0;

  const submit = (event: Event) => {
    event.preventDefault();
    onSubmit();
  };

  const setTaxpayer = (key: keyof TaxpayerForm, value: string) =>
    onChange({ ...inputs, taxpayer: { ...inputs.taxpayer, [key]: value } });

  return (
    <form onSubmit={submit}>
      {/* While a run is sent, the whole form is disabled, so the result always belongs to the inputs shown. */}
      <fieldset class="run-form" disabled={sending}>
        <section class="run-section" aria-labelledby="files-title">
          <h2 id="files-title">{uiText(language, "filesTitle")}</h2>
          {brokerKinds.map((kind) => {
            const files = inputs.brokerFiles[kind.broker] ?? [];
            const areaLabels = labels.broker.slice(labelStart, labelStart + files.length);

            labelStart += files.length;

            return (
              <FileArea
                key={kind.broker}
                language={language}
                title={kind.title[language]}
                format={kind.format[language]}
                accept={kind.accept}
                hint={uiText(language, "brokerFilesHint")}
                files={files}
                labels={areaLabels}
                ordered
                onChange={(changed) =>
                  onChange({ ...inputs, brokerFiles: { ...inputs.brokerFiles, [kind.broker]: changed } })
                }
              />
            );
          })}
          <details class="other-files">
            <summary>{uiText(language, "otherFilesTitle")}</summary>
            <FileArea
              language={language}
              title={uiText(language, "rateFilesTitle")}
              hint={uiTextWith(language, "rateFilesHint", {
                first: FIRST_BUILT_IN_RATE_YEAR,
                last: LAST_BUILT_IN_RATE_YEAR,
              })}
              accept=".csv,text/csv"
              files={inputs.rateFiles}
              labels={labels.rate}
              ordered={false}
              onChange={(changed) => onChange({ ...inputs, rateFiles: changed })}
            />
            <FileArea
              language={language}
              title={uiText(language, "referenceFilesTitle")}
              hint={uiText(language, "referenceFilesHint")}
              accept=".csv,text/csv"
              files={inputs.referenceFiles}
              labels={labels.reference}
              ordered={false}
              onChange={(changed) => onChange({ ...inputs, referenceFiles: changed })}
            />
          </details>
        </section>

        <section class="run-section">
          <label class="field">
            <span>{uiText(language, "yearTitle")}</span>
            <select
              value={inputs.reportingYear}
              onChange={(event) => onChange({ ...inputs, reportingYear: Number(event.currentTarget.value) })}
            >
              {reportingYears(today).map((year) => (
                <option key={year} value={year}>
                  {year}
                </option>
              ))}
            </select>
          </label>
          {needsRateHint(inputs.reportingYear) ? <p class="hint">{uiText(language, "yearRateHint")}</p> : null}
          <fieldset class="field">
            <legend>{uiText(language, "filingTitle")}</legend>
            {(["original", "self_report"] as FilingType[]).map((type) => (
              <label key={type} class="radio">
                <input
                  type="radio"
                  name="filingType"
                  value={type}
                  checked={inputs.filingType === type}
                  onChange={() => onChange({ ...inputs, filingType: type })}
                />
                {uiText(language, type === "original" ? "filingOriginal" : "filingSelfReport")}
              </label>
            ))}
          </fieldset>
        </section>

        <section class="run-section" aria-labelledby="taxpayer-title">
          <h2 id="taxpayer-title">{uiText(language, "taxpayerTitle")}</h2>
          <p class="hint">{uiText(language, "taxpayerResidentNote")}</p>
          {taxpayerFields.map((spec) => {
            const invalid = fieldError?.field === spec.key;
            const errorId = `error-${spec.key}`;
            const hintId = `hint-${spec.key}`;
            // A soft hint: it never blocks the button, the server only warns.
            const checkDigitHint =
              spec.key === "taxNumber" && !invalid && hasWrongCheckDigit(inputs.taxpayer.taxNumber);

            return (
              <label key={spec.key} class="field">
                <span>{uiText(language, spec.label)}</span>
                <input
                  type={spec.type}
                  value={inputs.taxpayer[spec.key]}
                  inputMode={spec.inputMode}
                  maxLength={spec.maxLength}
                  required={spec.required}
                  aria-invalid={invalid ? "true" : undefined}
                  aria-describedby={invalid ? errorId : checkDigitHint ? hintId : undefined}
                  onInput={(event) => setTaxpayer(spec.key, event.currentTarget.value)}
                />
                {invalid ? (
                  <span class="field-error" id={errorId}>
                    {fieldError?.text}
                  </span>
                ) : null}
                {checkDigitHint ? (
                  <span class="hint" id={hintId}>
                    {uiText(language, "taxpayerHintTaxNumberCheckDigit")}
                  </span>
                ) : null}
              </label>
            );
          })}
          {rememberBox}
        </section>

        <button type="submit" class="button" disabled={sending || !hasBrokerFile || submitBlocked}>
          {uiText(language, sending ? "sending" : outOfDate ? "createReportsAgain" : "createReports")}
        </button>
      </fieldset>
    </form>
  );
}
