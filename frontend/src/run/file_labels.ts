import { orderedBrokerFiles, type RunInputs } from "./inputs";

/** The default suffix of a repeated name; the page passes a translated one. */
const englishSuffix = (number: number): string => ` (file ${number})`;

/**
 * The names to show for the broker files. A name that occurs more than once gets a suffix with
 * the file number (the position in the request + 1, the number of `source-000N` in the
 * diagnostics), so two files named `taxreport.csv` can be told apart. Other names stay as they are.
 */
export function fileLabels(names: string[], suffix: (number: number) => string = englishSuffix): string[] {
  const counts = new Map<string, number>();

  for (const name of names) {
    counts.set(name, (counts.get(name) ?? 0) + 1);
  }

  return names.map((name, index) => ((counts.get(name) ?? 0) > 1 ? name + suffix(index + 1) : name));
}

/** The names shown for each kind of file; only the broker files get a file number. */
export interface InputLabels {
  broker: string[];
  rate: string[];
  reference: string[];
}

/**
 * The names of all chosen files. The broker labels follow the request order of all broker areas;
 * rate and reference files have no source ID, so they never get a number.
 */
export function inputLabels(inputs: RunInputs, suffix?: (number: number) => string): InputLabels {
  return {
    broker: fileLabels(orderedBrokerFiles(inputs).map((entry) => entry.file.name), suffix),
    rate: inputs.rateFiles.map((file) => file.name),
    reference: inputs.referenceFiles.map((file) => file.name),
  };
}
