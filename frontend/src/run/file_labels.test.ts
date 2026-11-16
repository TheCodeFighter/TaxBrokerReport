import { describe, expect, it } from "vitest";

import { emptyInputs } from "./inputs";
import { fileLabels, inputLabels } from "./file_labels";

describe("fileLabels", () => {
  it("adds the file number to a name that occurs more than once", () => {
    expect(fileLabels(["a.csv", "taxreport.csv", "taxreport.csv"])).toEqual([
      "a.csv",
      "taxreport.csv (file 2)",
      "taxreport.csv (file 3)",
    ]);
  });

  it("leaves different names as they are", () => {
    expect(fileLabels(["a.csv", "b.csv"])).toEqual(["a.csv", "b.csv"]);
  });

  it("numbers the repeated names from the start of the list", () => {
    expect(fileLabels(["x.csv", "x.csv"])).toEqual(["x.csv (file 1)", "x.csv (file 2)"]);
  });

  it("uses the suffix it is given", () => {
    expect(fileLabels(["x.csv", "x.csv"], (number) => ` [datoteka ${number}]`)).toEqual([
      "x.csv [datoteka 1]",
      "x.csv [datoteka 2]",
    ]);
  });
});

describe("inputLabels", () => {
  it("numbers only the broker files, not the rate and reference files with the same name", () => {
    const inputs = emptyInputs(new Date(2026, 10, 16));

    inputs.brokerFiles.trade_republic = [new File(["1"], "x.csv"), new File(["2"], "x.csv")];
    inputs.rateFiles = [new File(["3"], "x.csv")];
    inputs.referenceFiles = [new File(["4"], "x.csv")];

    expect(inputLabels(inputs)).toEqual({
      broker: ["x.csv (file 1)", "x.csv (file 2)"],
      rate: ["x.csv"],
      reference: ["x.csv"],
    });
  });
});
