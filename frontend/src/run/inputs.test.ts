import { describe, expect, it } from "vitest";

import { addFiles, emptyInputs, moveFile, orderedBrokerFiles, removeFile } from "./inputs";

describe("file list changes", () => {
  it("moves a file up and down", () => {
    expect(moveFile(["a", "b", "c"], 2, "up")).toEqual(["a", "c", "b"]);
    expect(moveFile(["a", "b", "c"], 0, "down")).toEqual(["b", "a", "c"]);
  });

  it("leaves the list as it is at the top and at the bottom", () => {
    expect(moveFile(["a", "b", "c"], 0, "up")).toEqual(["a", "b", "c"]);
    expect(moveFile(["a", "b", "c"], 2, "down")).toEqual(["a", "b", "c"]);
  });

  it("removes one file and adds files at the end", () => {
    expect(removeFile(["a", "b", "c"], 1)).toEqual(["a", "c"]);
    expect(addFiles(["a"], ["b", "c"])).toEqual(["a", "b", "c"]);
  });
});

describe("emptyInputs", () => {
  it("starts with no files, the year before this one and an original filing", () => {
    const inputs = emptyInputs(new Date(2026, 10, 16));

    expect(inputs.brokerFiles).toEqual({ trade_republic: [] });
    expect(inputs.rateFiles).toEqual([]);
    expect(inputs.referenceFiles).toEqual([]);
    expect(inputs.reportingYear).toBe(2025);
    expect(inputs.filingType).toBe("original");
    expect(inputs.taxpayer.taxNumber).toBe("");
  });
});

describe("orderedBrokerFiles", () => {
  it("keeps the order of the area and names the broker", () => {
    const inputs = emptyInputs(new Date(2026, 10, 16));
    const first = new File(["a"], "b.csv");
    const second = new File(["b"], "a.csv");

    inputs.brokerFiles.trade_republic = [first, second];

    expect(orderedBrokerFiles(inputs).map((entry) => [entry.file.name, entry.broker])).toEqual([
      ["b.csv", "trade_republic"],
      ["a.csv", "trade_republic"],
    ]);
  });
});
