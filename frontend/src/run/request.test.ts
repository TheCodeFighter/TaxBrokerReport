import { describe, expect, it } from "vitest";

import { emptyInputs } from "./inputs";
import { buildRunRequest } from "./request";

function inputsWithFiles() {
  const inputs = emptyInputs(new Date(2026, 10, 16));

  inputs.brokerFiles.trade_republic = [new File(["1"], "a.csv"), new File(["2"], "b.csv")];
  inputs.rateFiles = [new File(["3"], "r.csv")];
  inputs.taxpayer.taxNumber = "12345679";
  inputs.taxpayer.name = "Synthetic Taxpayer";

  return inputs;
}

describe("buildRunRequest", () => {
  it("sends the request part first and the files in the order broker, rates, reference", async () => {
    const form = buildRunRequest(inputsWithFiles());

    expect([...form.keys()]).toEqual(["request", "file0", "file1", "file2"]);
    expect(JSON.parse(form.get("request") as string)).toEqual({
      reportingYear: 2025,
      filingType: "original",
      taxpayer: { taxNumber: "12345679", name: "Synthetic Taxpayer", isResident: true },
      files: [
        { part: "file0", kind: "broker", broker: "trade_republic" },
        { part: "file1", kind: "broker", broker: "trade_republic" },
        { part: "file2", kind: "ecb_rates" },
      ],
    });
    expect((form.get("file0") as File).name).toBe("a.csv");
    expect(await (form.get("file1") as File).text()).toBe("2");
    expect((form.get("file2") as File).name).toBe("r.csv");
  });

  it("names the broker of each file, Trade Republic first and Interactive Brokers second", () => {
    const inputs = emptyInputs(new Date(2026, 10, 16));

    inputs.brokerFiles.interactive_brokers = [new File(["x"], "flex.xml")];
    inputs.brokerFiles.trade_republic = [new File(["a"], "tr.csv")];

    const form = buildRunRequest(inputs);

    expect(JSON.parse(form.get("request") as string).files).toEqual([
      { part: "file0", kind: "broker", broker: "trade_republic" },
      { part: "file1", kind: "broker", broker: "interactive_brokers" },
    ]);
    expect((form.get("file0") as File).name).toBe("tr.csv");
    expect((form.get("file1") as File).name).toBe("flex.xml");
  });

  it("sends a self-report and, with the switch off, no developer mode key", () => {
    const inputs = inputsWithFiles();

    inputs.filingType = "self_report";
    inputs.referenceFiles = [new File(["4"], "p.csv")];

    const request = JSON.parse(buildRunRequest(inputs).get("request") as string);

    expect(request.filingType).toBe("self_report");
    expect(Object.keys(request)).not.toContain("developerMode");
    expect(request.files[3]).toEqual({ part: "file3", kind: "reference" });
  });

  it("sends developerMode true only when the switch is on", () => {
    const inputs = inputsWithFiles();

    inputs.developerMode = true;

    const request = JSON.parse(buildRunRequest(inputs).get("request") as string);

    expect(request.developerMode).toBe(true);
  });
});
