import { orderedBrokerFiles, type RunInputs } from "./inputs";
import { taxpayerObject } from "./taxpayer";

/** One entry of the `files` list of the request part. */
interface FileEntry {
  part: string;
  kind: "broker" | "ecb_rates" | "reference";
  broker?: string;
}

/**
 * The upload of a run: the part `request` first, then one part per file, `file0` and on, in the
 * order broker files, ECB rate files, reference files. The order of the broker files is the order
 * of their source IDs. `developerMode` is sent only when the switch is on, so a normal run never
 * carries the key.
 */
export function buildRunRequest(inputs: RunInputs): FormData {
  const files: FileEntry[] = [];
  const parts: { part: string; file: File }[] = [];

  const add = (file: File, entry: Omit<FileEntry, "part">) => {
    const part = `file${parts.length}`;

    parts.push({ part, file });
    files.push({ part, ...entry });
  };

  for (const { file, broker } of orderedBrokerFiles(inputs)) {
    add(file, { kind: "broker", broker });
  }

  for (const file of inputs.rateFiles) {
    add(file, { kind: "ecb_rates" });
  }

  for (const file of inputs.referenceFiles) {
    add(file, { kind: "reference" });
  }

  const form = new FormData();

  form.append(
    "request",
    JSON.stringify({
      reportingYear: inputs.reportingYear,
      filingType: inputs.filingType,
      taxpayer: taxpayerObject(inputs.taxpayer),
      ...(inputs.developerMode ? { developerMode: true } : {}),
      files,
    }),
  );

  for (const { part, file } of parts) {
    form.append(part, file, file.name);
  }

  return form;
}
