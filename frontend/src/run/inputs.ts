import { brokerKinds } from "./input_kinds";
import { emptyTaxpayerForm, type TaxpayerForm } from "./taxpayer";
import { defaultYear } from "./years";

export type FilingType = "original" | "self_report";

/** Everything the user chose for one run. The files stay in the browser until the run is sent. */
export interface RunInputs {
  /** The broker files of each broker area, in the order the user set. */
  brokerFiles: Record<string, File[]>;
  rateFiles: File[];
  referenceFiles: File[];
  reportingYear: number;
  filingType: FilingType;
  taxpayer: TaxpayerForm;
  /** Developer mode of this run: it may offer an unsafe Doh-KDVP. Off at every start, never saved. */
  developerMode: boolean;
}

export function emptyInputs(today: Date): RunInputs {
  return {
    brokerFiles: Object.fromEntries(brokerKinds.map((kind) => [kind.broker, []])),
    rateFiles: [],
    referenceFiles: [],
    reportingYear: defaultYear(today),
    filingType: "original",
    taxpayer: { ...emptyTaxpayerForm },
    developerMode: false,
  };
}

/** The files after adding more at the end. */
export function addFiles<T>(list: readonly T[], added: readonly T[]): T[] {
  return [...list, ...added];
}

export function removeFile<T>(list: readonly T[], index: number): T[] {
  return list.filter((_item, position) => position !== index);
}

/** Moves one file a step up or down. At the top or bottom the list is returned unchanged. */
export function moveFile<T>(list: readonly T[], index: number, direction: "up" | "down"): T[] {
  const target = direction === "up" ? index - 1 : index + 1;

  if (index < 0 || index >= list.length || target < 0 || target >= list.length) {
    return [...list];
  }

  const moved = [...list];
  const item = list[index] as T;

  moved[index] = list[target] as T;
  moved[target] = item;

  return moved;
}

/** A broker file with its broker, in request order: the position is the file number. */
export interface BrokerFileEntry {
  file: File;
  broker: string;
}

/** The broker files of all areas in the order of the broker table, then the order of the area. */
export function orderedBrokerFiles(inputs: RunInputs): BrokerFileEntry[] {
  return brokerKinds.flatMap((kind) =>
    (inputs.brokerFiles[kind.broker] ?? []).map((file) => ({ file, broker: kind.broker })),
  );
}
